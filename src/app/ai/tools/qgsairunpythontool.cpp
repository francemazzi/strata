/***************************************************************************
    qgsairunpythontool.cpp
    ---------------------
    begin                : April 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsairunpythontool.h"

#include <algorithm>

#include "qgsaiauditlog.h"
#include "qgsaipythonapprovaldialog.h"
#include "qgsaipythonruntime.h"
#include "qgsaitaskrunner.h"
#include "qgsaitoolschemautil.h"
#include "qgsaiworkspacetrust.h"
#include "qgsfeedback.h"
#include "qgsmessagelog.h"
#include "qgsprocessingfeedback.h"
#include "qgspythonrunner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace
{
  // Python source literal that performs sandboxed-ish execution: redirects
  // stdout/stderr, runs the user's code via exec(), serialises everything as
  // JSON to a temp file. The %1/%2 placeholders are Python path literals and
  // %3 is the indented runtime namespace bootstrap.
  // Keep this compatible with Python 3.7+.
  constexpr const char *PY_WRAPPER_TEMPLATE = R"(
import sys, traceback, json, io

__qgsai_code_path = %1
__qgsai_out_path = %2

with open(__qgsai_code_path, "r", encoding="utf-8") as __qgsai_f:
    __qgsai_code = __qgsai_f.read()

__qgsai_stdout = io.StringIO()
__qgsai_stderr = io.StringIO()
__qgsai_old_stdout = sys.stdout
__qgsai_old_stderr = sys.stderr
sys.stdout = __qgsai_stdout
sys.stderr = __qgsai_stderr
__qgsai_error = ""
__qgsai_exception_type = ""
__qgsai_exception_message = ""
try:
    from strata_ai.run_python import session as __qgsai_session
except Exception as __qgsai_import_error:
    import warnings as __qgsai_warnings
    __qgsai_warnings.warn("strata_ai.run_python is unavailable: %s" % __qgsai_import_error)
    class __qgsai_session:
        def __init__(self, *args, **kwargs):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return False

try:
%3
    with __qgsai_session(%4, %5):
        exec(compile(__qgsai_code, "<ai_run_python>", "exec"), globals())
except SystemExit as __qgsai_ex:
    if __qgsai_ex.code not in (None, 0):
        __qgsai_exception_type = type(__qgsai_ex).__name__
        __qgsai_exception_message = str(__qgsai_ex.code)
        __qgsai_error = traceback.format_exc()
except BaseException as __qgsai_ex:
    __qgsai_exception_type = type(__qgsai_ex).__name__
    __qgsai_exception_message = str(__qgsai_ex)
    __qgsai_error = traceback.format_exc()
sys.stdout = __qgsai_old_stdout
sys.stderr = __qgsai_old_stderr

try:
    with open(__qgsai_out_path, "w", encoding="utf-8") as __qgsai_f:
        json.dump({
            "stdout": __qgsai_stdout.getvalue(),
            "stderr": __qgsai_stderr.getvalue(),
            "error": __qgsai_error,
            "exception_type": __qgsai_exception_type,
            "exception_message": __qgsai_exception_message,
        }, __qgsai_f)
except BaseException:
    if not __qgsai_error:
        __qgsai_error = traceback.format_exc()
)";

  /**
   * Encodes \a path as a Python source-level string literal: opens with single
   * quotes, escapes embedded backslashes and single-quotes. Robust on Windows
   * where the path contains backslashes that would otherwise be interpreted
   * as escape sequences inside the embedded Python.
   */
  QString escapeRunPythonPath( const QString &path )
  {
    QString escaped = path;
    escaped.replace( '\\', "\\\\"_L1 );
    escaped.replace( '\'', "\\'"_L1 );
    return u"'%1'"_s.arg( escaped );
  }

  QString truncateRunPythonOutput( const QString &text, int maxBytes )
  {
    const QByteArray utf8 = text.toUtf8();
    if ( utf8.size() <= maxBytes )
      return text;
    return QString::fromUtf8( utf8.left( maxBytes ) ) + u"\n…[truncated]"_s;
  }

  void appendRunPythonDiagnostic( QJsonArray &diagnostics, const QString &code, const QString &source, const QString &message )
  {
    for ( const QJsonValue &value : diagnostics )
    {
      if ( value.toObject().value( u"code"_s ).toString() == code )
        return;
    }

    QJsonObject diagnostic;
    diagnostic.insert( u"code"_s, code );
    diagnostic.insert( u"source"_s, source );
    diagnostic.insert( u"message"_s, message.trimmed() );
    diagnostics.append( diagnostic );
  }

  void diagnoseExplicitJsonFailure( const QString &text, const QString &source, QJsonArray &diagnostics )
  {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson( text.trimmed().toUtf8(), &parseError );
    if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
      return;

    const QJsonObject object = document.object();
    const QString status = object.value( u"status"_s ).toString().trimmed().toLower();
    const bool failedStatus = status == "error"_L1 || status == "failed"_L1 || status == "failure"_L1;
    const bool failedBoolean = ( object.value( u"success"_s ).isBool() && !object.value( u"success"_s ).toBool() ) || ( object.value( u"ok"_s ).isBool() && !object.value( u"ok"_s ).toBool() );
    if ( !failedStatus && !failedBoolean )
      return;

    QString message = object.value( u"message"_s ).toString().trimmed();
    if ( message.isEmpty() )
      message = object.value( u"error"_s ).toString().trimmed();
    if ( message.isEmpty() )
      message = u"Python code returned an explicit failed status."_s;
    appendRunPythonDiagnostic( diagnostics, u"explicit_failure"_s, source, message );
  }
} //namespace

QgsAiRunPythonTool::QgsAiRunPythonTool( QWidget *dialogParent )
  : mDialogParent( dialogParent )
{}

void QgsAiRunPythonTool::setRememberApprovalsForSession( bool enabled )
{
  if ( mRememberApprovalsForSession == enabled )
    return;

  mRememberApprovalsForSession = enabled;
  if ( !enabled )
    mLowRiskApprovalGrantedForSession = false;
}

void QgsAiRunPythonTool::setTimeoutSeconds( int seconds )
{
  mTimeoutSeconds = std::clamp( seconds, 10, 3600 );
}

bool QgsAiRunPythonTool::isAvailable() const
{
  return QgsPythonRunner::isValid() && QgsAiWorkspaceTrust::isCurrentWorkspaceTrusted();
}

QString QgsAiRunPythonTool::availabilityReason() const
{
  if ( !QgsPythonRunner::isValid() )
  {
    const QString reason = QgsPythonRunner::unavailableReason();
    return reason.isEmpty()
             ? u"Python runner is not available in this QGIS instance. Start QGIS with Python enabled (do not use --nopython), build with WITH_BINDINGS, and verify that the qgispython support library loads."_s
             : reason;
  }
  return u"run_python is disabled because this workspace is not trusted. Trust the workspace from the AI provider settings to enable Python execution."_s;
}

QString QgsAiRunPythonTool::description() const
{
  return QStringLiteral(
    "Executes a snippet of PyQGIS code in the running QGIS session. "
    "Captures stdout/stderr and any Python traceback. The user must approve "
    "high-risk code via a modal dialog before it runs; refusal returns 'user_rejected'. "
    "The execution namespace already provides iface, processing, qgis and PyQt "
    "(qgis.PyQt), and includes AI-installed profile packages on sys.path. "
    "Use this tool ONLY when the action genuinely requires Python (e.g. driving "
    "the QGIS API to add a runtime layer). Prefer propose_edit/propose_create_file "
    "for static file changes. Prefer calculate_field, batch_update_attributes or "
    "run_processing_algorithm instead of Python feature loops: they run in the background "
    "and can be stopped. Python execution stops after a time budget (120 s by default, "
    "configurable in the AI settings); time spent inside processing.run does not count, "
    "and processing.run keeps the window responsive and honors Stop."
  );
}

QJsonObject QgsAiRunPythonTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"code"_s, prop( u"string"_s, u"The PyQGIS code to execute. Maximum 8000 characters."_s ) );
  properties.insert( u"description"_s, prop( u"string"_s, u"Short human-readable explanation of what the code does. Shown in the approval dialog."_s ) );
  return schemaObject( properties, QJsonArray { u"code"_s } );
}

QJsonObject QgsAiRunPythonTool::diagnoseCapturedOutput( const QString &stdoutText, const QString &stderrText, const QString &tracebackText, const QString &exceptionType, const QString &exceptionMessage )
{
  QJsonArray diagnostics;
  if ( !tracebackText.trimmed().isEmpty() )
  {
    const QString message = exceptionMessage.trimmed().isEmpty() ? tracebackText.trimmed() : exceptionMessage.trimmed();
    appendRunPythonDiagnostic( diagnostics, u"python_exception"_s, u"traceback"_s, message );
  }

  const QList<QPair<QString, QString>> streams {
    { u"stdout"_s, stdoutText },
    { u"stderr"_s, stderrText },
  };
  const QRegularExpression serviceExceptionExpression( u"(?:<\\s*(?:\\w+:)?ServiceException(?:Report)?\\b|\\bServiceException(?:Report)?\\b)"_s, QRegularExpression::CaseInsensitiveOption );
  const QRegularExpression
    invalidProviderExpression( u"\\b(?:provider\\s+(?:is\\s+)?(?:invalid|not\\s+valid|unavailable)|invalid\\s+provider|layer\\s+(?:is\\s+)?not\\s+valid)\\b"_s, QRegularExpression::CaseInsensitiveOption );
  const QRegularExpression explicitFailureExpression( u"^\\s*(?:FAILED|FAILURE)\\s*:\\s*(.+)$"_s, QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption );

  for ( const auto &stream : streams )
  {
    const QString &source = stream.first;
    const QString &text = stream.second;
    const QRegularExpressionMatch serviceMatch = serviceExceptionExpression.match( text );
    if ( serviceMatch.hasMatch() )
      appendRunPythonDiagnostic( diagnostics, u"service_exception"_s, source, u"OGC service returned a ServiceException. Verify the endpoint, requested layer/type name, parameters, and service capabilities."_s );

    const QRegularExpressionMatch providerMatch = invalidProviderExpression.match( text );
    if ( providerMatch.hasMatch() )
      appendRunPythonDiagnostic( diagnostics, u"invalid_provider"_s, source, providerMatch.captured( 0 ) );

    const QRegularExpressionMatch failureMatch = explicitFailureExpression.match( text );
    if ( failureMatch.hasMatch() )
      appendRunPythonDiagnostic( diagnostics, u"explicit_failure"_s, source, failureMatch.captured( 1 ) );

    diagnoseExplicitJsonFailure( text, source, diagnostics );
  }

  QJsonObject result;
  result.insert( u"status"_s, diagnostics.isEmpty() ? u"ok"_s : u"error"_s );
  result.insert( u"diagnostics"_s, diagnostics );
  if ( !diagnostics.isEmpty() )
  {
    const QJsonObject primary = diagnostics.at( 0 ).toObject();
    result.insert( u"failure_code"_s, primary.value( u"code"_s ) );
    result.insert( u"failure_message"_s, primary.value( u"message"_s ) );
  }
  if ( !exceptionType.trimmed().isEmpty() )
    result.insert( u"exception_type"_s, exceptionType.trimmed() );
  return result;
}

QStringList QgsAiRunPythonTool::featureLoopHints( const QString &code )
{
  // Editing features one by one in Python runs on the interface thread and is the usual
  // reason a snippet hits its time budget. Reading features in a loop is fine.
  static const QRegularExpression featureIteration( uR"(\bgetFeatures\s*\()"_s );
  static const QRegularExpression featureMutation( uR"(\b(?:changeAttributeValues?|changeGeometry|updateFeature|deleteFeatures?|setAttribute)\s*\()"_s );
  static const QRegularExpression loop( uR"(\b(?:for|while)\b)"_s );
  if ( featureIteration.match( code ).hasMatch() && featureMutation.match( code ).hasMatch() && loop.match( code ).hasMatch() )
    return { u"slow_feature_loop"_s };
  return {};
}

QString QgsAiRunPythonTool::hintMessage( const QString &hint )
{
  if ( hint == "slow_feature_loop"_L1 )
    return u"This snippet edits features one by one in a Python loop, which blocks Strata. For whole-layer updates prefer calculate_field or batch_update_attributes: they run in the background and can be stopped."_s;
  return QString();
}

QgsAiToolResult QgsAiRunPythonTool::execute( const QJsonObject &args )
{
  const QString code = args.value( u"code"_s ).toString();
  if ( code.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'code' is required and must be non-empty."_s );
  if ( code.size() > MAX_CODE_CHARS )
    return QgsAiToolResult::error( u"Refusing to run code exceeding %1 characters (got %2). Split the work into smaller calls."_s.arg( MAX_CODE_CHARS ).arg( code.size() ) );

  if ( !QgsPythonRunner::isValid() )
    return QgsAiToolResult::error( availabilityReason() );

  const QString description = args.value( u"description"_s ).toString();
  const QStringList riskMarkers = QgsAiPythonApprovalDialog::detectRiskMarkers( code );
  const bool hasRiskMarkers = !riskMarkers.isEmpty();
  const bool approvedBySessionGrant = mRememberApprovalsForSession && mLowRiskApprovalGrantedForSession && !hasRiskMarkers;
  if ( !approvedBySessionGrant )
  {
    QgsAiPythonApprovalDialog dialog( description, code, mRememberApprovalsForSession && !hasRiskMarkers, mDialogParent );
    const int dialogResult = dialog.exec();
    if ( dialogResult != QDialog::Accepted )
    {
      QgsMessageLog::logMessage( u"run_python rejected by user (codeChars=%1)"_s.arg( code.size() ), u"AI/Python"_s, Qgis::MessageLevel::Info, false );
      QJsonObject output;
      output.insert( u"status"_s, u"user_rejected"_s );
      return QgsAiToolResult::ok( output );
    }
    if ( mRememberApprovalsForSession && !hasRiskMarkers )
      mLowRiskApprovalGrantedForSession = true;
  }
  else
  {
    QgsMessageLog::logMessage( u"run_python: using remembered low-risk Python approval for this session (codeChars=%1)"_s.arg( code.size() ), u"AI/Python"_s, Qgis::MessageLevel::Info, false );
  }

  // Persist user code and capture-output paths to disk so the wrapper can read
  // them back without us worrying about Python-source quoting.
  const QString uniqueId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  const QString tmpDir = QDir::tempPath();
  const QString codePath = QDir( tmpDir ).filePath( u"qgsai_pycode_%1.py"_s.arg( uniqueId ) );
  const QString outPath = QDir( tmpDir ).filePath( u"qgsai_pyout_%1.json"_s.arg( uniqueId ) );
  const QString wrapperPath = QDir( tmpDir ).filePath( u"qgsai_pywrapper_%1.py"_s.arg( uniqueId ) );

  {
    QFile codeFile( codePath );
    if ( !codeFile.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
      return QgsAiToolResult::error( u"Cannot write temp code file: %1"_s.arg( codePath ) );
    codeFile.write( code.toUtf8() );
  }

  QgsMessageLog::logMessage( u"run_python: executing approved code (codeChars=%1)"_s.arg( code.size() ), u"AI/Python"_s, Qgis::MessageLevel::Info, false );
  QgsAiAuditLog::append( u"run_python"_s, code );

  // Stop cancels this bridge; Python cancels its Processing runs from it and stops the snippet.
  QgsProcessingFeedback feedback;
  const QgsAiActiveFeedbackScope activeFeedback( &feedback, u"run_python"_s );

  // Build the wrapper with safely-quoted paths. One arg() call, so a '%' sequence inside a
  // substituted path or bootstrap is never read as a later placeholder.
  const QString wrapper = QString::fromUtf8( PY_WRAPPER_TEMPLATE )
                            .arg(
                              escapeRunPythonPath( codePath ),
                              escapeRunPythonPath( outPath ),
                              QgsAiPythonRuntime::bootstrapSource( QgsAiPythonRuntime::packageTargetPath() ),
                              QString::number( static_cast<qulonglong>( reinterpret_cast<quintptr>( &feedback ) ) ),
                              QString::number( mTimeoutSeconds )
                            );

  {
    QFile wrapperFile( wrapperPath );
    if ( !wrapperFile.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
      return QgsAiToolResult::error( u"Cannot write temp wrapper file: %1"_s.arg( wrapperPath ) );
    wrapperFile.write( wrapper.toUtf8() );
  }

  QString runnerError;
  const bool ranOk = QgsPythonRunner::runFileCaptureError( wrapperPath, runnerError );

  QString stdoutText;
  QString stderrText;
  QString tracebackText;
  QString exceptionType;
  QString exceptionMessage;
  if ( QFile::exists( outPath ) )
  {
    QFile outFile( outPath );
    if ( outFile.open( QIODevice::ReadOnly ) )
    {
      const QByteArray body = outFile.readAll();
      const QJsonDocument doc = QJsonDocument::fromJson( body );
      if ( doc.isObject() )
      {
        const QJsonObject obj = doc.object();
        stdoutText = obj.value( u"stdout"_s ).toString();
        stderrText = obj.value( u"stderr"_s ).toString();
        tracebackText = obj.value( u"error"_s ).toString();
        exceptionType = obj.value( u"exception_type"_s ).toString();
        exceptionMessage = obj.value( u"exception_message"_s ).toString();
      }
    }
  }

  // Best-effort cleanup of the temp files.
  QFile::remove( codePath );
  QFile::remove( outPath );
  QFile::remove( wrapperPath );

  if ( !ranOk )
  {
    QgsMessageLog::logMessage( u"run_python: QgsPythonRunner::runFileCaptureError() returned false (wrapper failed)."_s, u"AI/Python"_s, Qgis::MessageLevel::Warning, false );
    if ( activeFeedback.canceledByUser() )
      return QgsAiToolResult::canceledResult( u"Python execution was canceled."_s );
    const QString detail = !runnerError.isEmpty() ? runnerError : tracebackText;
    return QgsAiToolResult::error( u"Python wrapper failed to execute. %1"_s.arg( detail ) );
  }

  const QStringList hints = featureLoopHints( code );
  if ( activeFeedback.canceledByUser() )
    return QgsAiToolResult::canceledResult( u"Python execution was canceled."_s );
  if ( exceptionType == "RunPythonTimeout"_L1 )
  {
    QString message
      = u"run_python stopped after %1 s of Python (time inside processing.run does not count). Use calculate_field, batch_update_attributes or run_processing_algorithm for long GIS work."_s.arg(
        mTimeoutSeconds
      );
    for ( const QString &hint : hints )
      message += ' ' + hintMessage( hint );
    return QgsAiToolResult::error( message );
  }

  const QJsonObject diagnosis = diagnoseCapturedOutput( stdoutText, stderrText, tracebackText, exceptionType, exceptionMessage );
  const bool hadFailure = diagnosis.value( u"status"_s ).toString() == "error"_L1;
  QgsMessageLog::
    logMessage( u"run_python: completed (stdoutBytes=%1, stderrBytes=%2, failure=%3)"_s.arg( stdoutText.size() ).arg( stderrText.size() ).arg( hadFailure ), u"AI/Python"_s, hadFailure ? Qgis::MessageLevel::Warning : Qgis::MessageLevel::Info, false );

  QJsonObject output = diagnosis;
  output.insert( u"stdout"_s, truncateRunPythonOutput( stdoutText, MAX_CAPTURE_BYTES ) );
  output.insert( u"stderr"_s, truncateRunPythonOutput( stderrText, MAX_CAPTURE_BYTES ) );
  if ( !tracebackText.isEmpty() )
    output.insert( u"traceback"_s, truncateRunPythonOutput( tracebackText, MAX_CAPTURE_BYTES ) );
  if ( !hints.isEmpty() )
  {
    // Advice only: hints never turn a successful run into a failure.
    QJsonArray hintArray;
    for ( const QString &hint : hints )
      hintArray.push_back( QJsonObject { { u"code"_s, hint }, { u"message"_s, hintMessage( hint ) } } );
    output.insert( u"hints"_s, hintArray );
  }
  return QgsAiToolResult::ok( output );
}

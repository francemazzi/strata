/***************************************************************************
    qgsaipythonruntime.cpp
    ----------------------
    begin                : August 2026
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

#include "qgsaipythonruntime.h"

#include "qgsapplication.h"

#include <QDir>
#include <QString>

using namespace Qt::StringLiterals;

namespace
{
  QString pythonStringLiteral( const QString &value )
  {
    QString escaped = value;
    escaped.replace( '\\', "\\\\"_L1 );
    escaped.replace( '\'', "\\'"_L1 );
    return u"'%1'"_s.arg( escaped );
  }

  constexpr const char *PIP_WRAPPER_TEMPLATE = R"PYTHON(
import importlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import traceback

try:
    from importlib import metadata as importlib_metadata
except ImportError:
    import importlib_metadata

__qgsai_out_path = %1
__qgsai_args_path = %2
__qgsai_target_path = %3
__qgsai_cache_path = %4
__qgsai_timeout = %5

def __qgsai_text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)

def __qgsai_package_name(spec):
    return re.split(r"[<>=!~]", spec, maxsplit=1)[0].strip()

def __qgsai_import_candidates(package_name):
    normalized = package_name.replace("-", "_").replace(".", "_")
    return (normalized, normalized.lower())

def __qgsai_module_available(package_name):
    for module_name in __qgsai_import_candidates(package_name):
        try:
            if importlib.util.find_spec(module_name) is not None:
                return True
        except (ImportError, AttributeError, ValueError):
            pass
    try:
        importlib_metadata.distribution(package_name)
        return True
    except importlib_metadata.PackageNotFoundError:
        return False

def __qgsai_read_cache():
    try:
        with open(__qgsai_cache_path, "r", encoding="utf-8") as cache_file:
            data = json.load(cache_file)
        specs = data.get("successful_specs", [])
        return set(spec for spec in specs if isinstance(spec, str))
    except (OSError, ValueError, TypeError):
        return set()

def __qgsai_write_cache(specs):
    cache_dir = os.path.dirname(__qgsai_cache_path)
    os.makedirs(cache_dir, exist_ok=True)
    temporary_path = __qgsai_cache_path + ".tmp"
    with open(temporary_path, "w", encoding="utf-8") as cache_file:
        json.dump({"version": 1, "successful_specs": sorted(specs)}, cache_file)
    os.replace(temporary_path, __qgsai_cache_path)

def __qgsai_bundled_python():
    application_dir = os.path.dirname(os.path.abspath(sys.executable))
    real_application_dir = os.path.realpath(application_dir)
    names = ["python", "python3", "python.exe", "python3.exe"]
    versioned_name = "python%s.%s" % (sys.version_info[0], sys.version_info[1])
    names.extend([versioned_name, versioned_name + ".exe"])
    candidates = []
    seen = set()
    for name in names:
        path = os.path.join(application_dir, name)
        real_path = os.path.realpath(path)
        if real_path in seen:
            continue
        seen.add(real_path)
        candidates.append(path)
        if os.path.dirname(real_path) != real_application_dir:
            continue
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            continue
        probe = (
            "import importlib.util, json, sys\n"
            "print(json.dumps({'executable': sys.executable, "
            "'version': [sys.version_info[0], sys.version_info[1]], "
            "'pip_importable': importlib.util.find_spec('pip') is not None}))\n"
        )
        try:
            process = subprocess.run(
                [path, "-c", probe],
                capture_output=True,
                text=True,
                timeout=15,
            )
            data = json.loads(process.stdout) if process.returncode == 0 else {}
        except BaseException:
            continue
        if data.get("version") != [sys.version_info[0], sys.version_info[1]]:
            continue
        if not data.get("pip_importable", False):
            continue
        if os.path.dirname(os.path.realpath(data.get("executable", ""))) != real_application_dir:
            continue
        return path, candidates
    return "", candidates

def __qgsai_is_pep668_error(stderr):
    lowered = stderr.lower()
    return (
        "externally-managed-environment" in lowered
        or "externally managed environment" in lowered
        or "pep 668" in lowered
    )

__qgsai_error = ""
__qgsai_stdout = ""
__qgsai_stderr = ""
__qgsai_returncode = -1
__qgsai_python_used = ""
__qgsai_python_candidates = []
__qgsai_installed = []
__qgsai_already_available = []
__qgsai_cached = []
__qgsai_retryable = True
__qgsai_error_code = ""
__qgsai_command = []
__qgsai_pending = []

try:
    os.makedirs(__qgsai_target_path, exist_ok=True)
    if __qgsai_target_path not in sys.path:
        sys.path.insert(0, __qgsai_target_path)
    importlib.invalidate_caches()

    with open(__qgsai_args_path, "r", encoding="utf-8") as args_file:
        __qgsai_arguments = json.load(args_file)
    # A list of specs, or {"packages": [...], "known_python": path, "prepare_only": bool}: with
    # prepare_only the pip command is returned for Strata to run in the background.
    if isinstance(__qgsai_arguments, dict):
        __qgsai_packages = __qgsai_arguments.get("packages", [])
        __qgsai_known_python = __qgsai_arguments.get("known_python", "")
        __qgsai_prepare_only = bool(__qgsai_arguments.get("prepare_only", False))
    else:
        __qgsai_packages = __qgsai_arguments
        __qgsai_known_python = ""
        __qgsai_prepare_only = False

    __qgsai_cache = __qgsai_read_cache()
    for __qgsai_spec in __qgsai_packages:
        __qgsai_name = __qgsai_package_name(__qgsai_spec)
        __qgsai_available = __qgsai_module_available(__qgsai_name)
        if __qgsai_spec in __qgsai_cache and __qgsai_available:
            __qgsai_cached.append(__qgsai_spec)
        elif __qgsai_available:
            __qgsai_already_available.append(__qgsai_spec)
            __qgsai_cache.add(__qgsai_spec)
        elif __qgsai_name.lower().replace("_", "-") == "duckdb":
            __qgsai_error_code = "bundled_package_missing"
            __qgsai_retryable = False
            raise RuntimeError("Bundled package 'duckdb' is not importable; refusing to reinstall it.")
        else:
            __qgsai_pending.append(__qgsai_spec)

    if not __qgsai_pending:
        __qgsai_returncode = 0
        __qgsai_write_cache(__qgsai_cache)
    else:
        if __qgsai_known_python and os.path.isfile(__qgsai_known_python):
            __qgsai_python_used, __qgsai_python_candidates = __qgsai_known_python, [__qgsai_known_python]
        else:
            __qgsai_python_used, __qgsai_python_candidates = __qgsai_bundled_python()
        if not __qgsai_python_used:
            __qgsai_error_code = "bundled_python_unavailable"
            __qgsai_retryable = False
            raise RuntimeError("No pip-enabled bundled Python interpreter was found beside the application.")

        __qgsai_command = [
            __qgsai_python_used,
            "-m",
            "pip",
            "install",
            "--target",
            __qgsai_target_path,
            "--disable-pip-version-check",
            *__qgsai_pending,
        ]
        __qgsai_environment = os.environ.copy()
        __qgsai_environment.pop("PIP_BREAK_SYSTEM_PACKAGES", None)
        __qgsai_environment.pop("PYTHONUSERBASE", None)
    if __qgsai_pending and __qgsai_prepare_only:
        __qgsai_write_cache(__qgsai_cache)
    elif __qgsai_pending:
        __qgsai_process = subprocess.run(
            __qgsai_command,
            capture_output=True,
            text=True,
            timeout=__qgsai_timeout,
            env=__qgsai_environment,
        )
        __qgsai_stdout = __qgsai_text(__qgsai_process.stdout)
        __qgsai_stderr = __qgsai_text(__qgsai_process.stderr)
        __qgsai_returncode = __qgsai_process.returncode
        if __qgsai_is_pep668_error(__qgsai_stderr):
            __qgsai_error_code = "externally_managed_environment"
            __qgsai_retryable = False
        if __qgsai_returncode == 0:
            __qgsai_installed = list(__qgsai_pending)
            __qgsai_cache.update(__qgsai_pending)
            __qgsai_write_cache(__qgsai_cache)
            importlib.invalidate_caches()
except subprocess.TimeoutExpired as __qgsai_exception:
    __qgsai_error = "pip install timed out after %s seconds" % __qgsai_timeout
    __qgsai_stdout = __qgsai_text(getattr(__qgsai_exception, "stdout", ""))
    __qgsai_stderr = __qgsai_text(getattr(__qgsai_exception, "stderr", ""))
except BaseException:
    __qgsai_error = traceback.format_exc()

with open(__qgsai_out_path, "w", encoding="utf-8") as output_file:
    json.dump({
        "stdout": __qgsai_stdout,
        "stderr": __qgsai_stderr,
        "returncode": __qgsai_returncode,
        "error": __qgsai_error,
        "error_code": __qgsai_error_code,
        "retryable": __qgsai_retryable,
        "python_used": __qgsai_python_used,
        "python_candidates": __qgsai_python_candidates,
        "target_path": __qgsai_target_path,
        "installed": __qgsai_installed,
        "already_available": __qgsai_already_available,
        "cached": __qgsai_cached,
        "pending": __qgsai_pending,
        "command": __qgsai_command if __qgsai_prepare_only else [],
    }, output_file)
)PYTHON";
} // namespace

QString QgsAiPythonRuntime::packageTargetPath()
{
  return QDir( QgsApplication::qgisSettingsDirPath() ).filePath( u"ai/python/site-packages"_s );
}

QString QgsAiPythonRuntime::installCachePath()
{
  return QDir( QgsApplication::qgisSettingsDirPath() ).filePath( u"ai/python/install-cache.json"_s );
}

QString QgsAiPythonRuntime::bootstrapSource( const QString &targetPath )
{
  return uR"PYTHON(    import importlib as __qgsai_importlib
    import sys as __qgsai_sys
    __qgsai_package_path = %1
    if __qgsai_package_path not in __qgsai_sys.path:
        __qgsai_sys.path.insert(0, __qgsai_package_path)
    __qgsai_importlib.invalidate_caches()
    import qgis
    import qgis.PyQt
    from qgis.utils import iface
    import processing
    PyQt = qgis.PyQt
)PYTHON"_s.arg( pythonStringLiteral( targetPath ) );
}

QString QgsAiPythonRuntime::pipWrapperSource( const QString &outputPath, const QString &argumentsPath, const QString &targetPath, const QString &cachePath, int timeoutSeconds )
{
  return QString::fromUtf8( PIP_WRAPPER_TEMPLATE )
    .arg( pythonStringLiteral( outputPath ), pythonStringLiteral( argumentsPath ), pythonStringLiteral( targetPath ), pythonStringLiteral( cachePath ), QString::number( timeoutSeconds ) );
}

bool QgsAiPythonRuntime::isExternallyManagedError( const QString &standardError )
{
  const QString lowered = standardError.toLower();
  return lowered.contains( u"externally-managed-environment"_s ) || lowered.contains( u"externally managed environment"_s ) || lowered.contains( u"pep 668"_s );
}

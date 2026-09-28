/***************************************************************************
    qgsaioauthcallbackpage.cpp
    ---------------------
    begin                : September 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/

#include "qgsaioauthcallbackpage.h"

#include <QFile>

using namespace Qt::StringLiterals;

QByteArray QgsAiOAuthCallbackPage::render( State state, const QString &title, const QString &message, const QString &detail )
{
  const bool success = state == State::Success;
  const QString tone = success ? u"success"_s : state == State::RateLimited ? u"warning"_s : u"error"_s;
  const QString icon = success ? u"✓"_s : state == State::RateLimited ? u"↻"_s : u"!"_s;
  const QString action = success ? QObject::tr( "Close this tab" ) : QObject::tr( "Return to Strata" );
  const QString hint = success ? QObject::tr( "Strata has already updated the connection." ) : QObject::tr( "This tab cannot retry the login safely." );
  const QString details = detail.isEmpty() ? QString() : u"<details><summary>%1</summary><code>%2</code></details>"_s.arg( QObject::tr( "Technical details" ).toHtmlEscaped(), detail.toHtmlEscaped() );
  const QString autoClose = success ? u"setTimeout(()=>window.close(),1400);"_s : QString();
  QFile logoFile( u":/images/icons/strata-icon-64x64.png"_s );
  const QString brandMark = logoFile.open( QIODevice::ReadOnly ) ? u"<img class=\"mark\" src=\"data:image/png;base64,%1\" alt=\"\">"_s.arg( QString::fromLatin1( logoFile.readAll().toBase64() ) )
                                                                 : u"<span class=\"mark mark-fallback\" aria-hidden=\"true\">S</span>"_s;

  const QString html = uR"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>%1 · Strata</title><style>
:root{color-scheme:light dark;--ink:#17211d;--muted:#59645f;--paper:#f4f2ec;--card:#fff;--line:#d8d9d2;--accent:#167a57;--glow:#9fe1c2}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:28px;background:radial-gradient(circle at 15%% 10%%,rgba(22,122,87,.12),transparent 34%%),linear-gradient(135deg,var(--paper),#e8ebe5);color:var(--ink);font-family:"Avenir Next","Segoe UI",sans-serif}.shell{width:min(620px,100%%);position:relative}.brand{display:flex;align-items:center;gap:11px;margin:0 0 16px 4px;font-weight:750;letter-spacing:.08em;text-transform:uppercase}.mark{display:block;width:32px;height:32px;border-radius:10px;object-fit:cover}.mark-fallback{display:grid;place-items:center;background:var(--ink);color:var(--paper);font-family:Georgia,serif;font-size:19px}.card{overflow:hidden;border:1px solid var(--line);border-radius:24px;background:color-mix(in srgb,var(--card) 94%%,transparent);box-shadow:0 28px 80px rgba(25,39,32,.16)}.stripe{height:6px;background:var(--status)}.content{padding:38px}.status{display:grid;place-items:center;width:58px;height:58px;border-radius:18px;background:color-mix(in srgb,var(--status) 14%%,transparent);color:var(--status);font-size:30px;font-weight:800}h1{margin:24px 0 10px;font:700 clamp(28px,6vw,42px)/1.05 Georgia,serif;letter-spacing:-.025em}p{margin:0;max-width:48ch;color:var(--muted);font-size:17px;line-height:1.6}.hint{margin-top:8px;font-size:14px}button{margin-top:28px;border:0;border-radius:12px;padding:13px 18px;background:var(--ink);color:var(--paper);font:700 15px/1 inherit;cursor:pointer}button:hover{transform:translateY(-1px);box-shadow:0 8px 24px rgba(23,33,29,.18)}details{margin-top:26px;padding-top:18px;border-top:1px solid var(--line);color:var(--muted)}summary{cursor:pointer;font-weight:650}code{display:block;margin-top:10px;white-space:pre-wrap;overflow-wrap:anywhere;font:13px/1.5 ui-monospace,monospace}.success{--status:#16835c}.warning{--status:#b16814}.error{--status:#b53c38}@media(prefers-color-scheme:dark){:root{--ink:#eef5f0;--muted:#adb9b2;--paper:#101713;--card:#17211c;--line:#344039}body{background:radial-gradient(circle at 15%% 10%%,rgba(80,189,139,.15),transparent 34%%),linear-gradient(135deg,#101713,#18231e)}button{background:#e7f2eb;color:#142019}}
</style></head><body><main class="shell %2"><div class="brand">%10Strata</div><section class="card" role="status" aria-live="polite"><div class="stripe"></div><div class="content"><div class="status" aria-hidden="true">%3</div><h1>%4</h1><p>%5</p><p class="hint">%6</p><button type="button" onclick="window.close()">%7</button>%8</div></section></main><script>history.replaceState(null,'','/callback');%9</script></body></html>)HTML"_s
                         .arg( title.toHtmlEscaped(), tone, icon, title.toHtmlEscaped(), message.toHtmlEscaped(), hint.toHtmlEscaped(), action.toHtmlEscaped(), details, autoClose, brandMark );
  return html.toUtf8();
}

QByteArray QgsAiOAuthCallbackPage::httpResponse( int status, const QByteArray &body )
{
  const QByteArray reason = status == 200 ? "OK" : status == 429 ? "Too Many Requests" : status >= 500 ? "Bad Gateway" : "Bad Request";
  QByteArray response = "HTTP/1.1 " + QByteArray::number( status ) + ' ' + reason + "\r\n";
  response += "Content-Type: text/html; charset=utf-8\r\n";
  response += "Cache-Control: no-store, max-age=0\r\nPragma: no-cache\r\n";
  response += "Content-Security-Policy: default-src 'none'; img-src data:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
  response += "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
  response += "Content-Length: " + QByteArray::number( body.size() ) + "\r\n\r\n" + body;
  return response;
}

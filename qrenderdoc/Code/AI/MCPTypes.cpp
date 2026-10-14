/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "MCPTypes.h"

#include <QJsonDocument>
#include "Code/QRDUtils.h"

namespace MCP
{

QString ErrorCodeName(ErrorCode code)
{
  switch(code)
  {
    case ErrorCode::ParseError: return QStringLiteral("PARSE_ERROR");
    case ErrorCode::InvalidRequest: return QStringLiteral("INVALID_REQUEST");
    case ErrorCode::MethodNotFound: return QStringLiteral("METHOD_NOT_FOUND");
    case ErrorCode::InvalidParams: return QStringLiteral("INVALID_PARAMS");
    case ErrorCode::InternalError: return QStringLiteral("INTERNAL_ERROR");
    case ErrorCode::NoCaptureLoaded: return QStringLiteral("NO_CAPTURE_LOADED");
    case ErrorCode::CaptureBusy: return QStringLiteral("CAPTURE_BUSY");
    case ErrorCode::ReplayUnavailable: return QStringLiteral("REPLAY_UNAVAILABLE");
    case ErrorCode::InvalidToolInput: return QStringLiteral("INVALID_TOOL_INPUT");
    case ErrorCode::SideEffectNotConfirmed: return QStringLiteral("SIDE_EFFECT_NOT_CONFIRMED");
    case ErrorCode::ProtocolVersionUnsupported:
      return QStringLiteral("PROTOCOL_VERSION_UNSUPPORTED");
    case ErrorCode::Cancelled: return QStringLiteral("CANCELLED");
    case ErrorCode::Timeout: return QStringLiteral("TIMEOUT");
    case ErrorCode::Unsupported: return QStringLiteral("UNSUPPORTED");
  }

  return QStringLiteral("UNKNOWN");
}

QJsonObject Error::toJson() const
{
  QJsonObject obj;
  obj[QStringLiteral("code")] = int(code);
  obj[QStringLiteral("message")] = message;

  // Always expose the stable string identifier so clients can branch on it
  // without depending on the numeric code layout.
  QJsonObject dataObj = data;
  dataObj[QStringLiteral("qrenderdocCode")] = ErrorCodeName(code);

  if(!dataObj.isEmpty())
    obj[QStringLiteral("data")] = dataObj;

  return obj;
}

Error Error::fromJson(const QJsonObject &obj)
{
  Error e;
  e.hasError = true;
  e.code = ErrorCode(obj[QStringLiteral("code")].toInt(int(ErrorCode::InternalError)));
  e.message = obj[QStringLiteral("message")].toString();

  QJsonObject dataObj = obj[QStringLiteral("data")].toObject();
  // Don't round-trip the derived identifier back into the payload.
  dataObj.remove(QStringLiteral("qrenderdocCode"));
  e.data = dataObj;

  return e;
}

ToolResult ToolResult::Success(const QJsonObject &structuredContent, const QString &textContent)
{
  ToolResult r;
  r.ok = true;
  r.structured = structuredContent;
  r.text = textContent.isEmpty() ? QString::fromUtf8(
                                       QJsonDocument(structuredContent).toJson(QJsonDocument::Compact))
                                 : textContent;
  return r;
}

ToolResult ToolResult::Failure(const Error &e)
{
  ToolResult r;
  r.ok = false;
  r.error = e;
  r.text = e.message;
  return r;
}

}    // namespace MCP

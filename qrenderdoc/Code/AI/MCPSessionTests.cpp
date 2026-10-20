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

// Native session contract tests. No simulated provider is used to claim injection,
// device transport or replay coverage; those require real application/device tests.
#include "Code/QRDUtils.h"

#if ENABLE_UNIT_TESTS
#include "3rdparty/catch/catch.hpp"
#include "MCPSessionTools.h"
#include "MCPQueryAdapter.h"
#include "MCPTools.h"
#include "MCPExternalTools.h"
#include <thread>
#include <chrono>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

// Same test-only formatter as MCPTests.cpp; RenderDoc's fallback ToStr does
// not define QString conversions. Keep the definition identical across units.
namespace Catch
{
template <>
struct StringMaker<QString>
{
  static std::string convert(const QString &value)
  {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constBegin(), utf8.constEnd());
  }
};
}

TEST_CASE("Native MCP application session registration preserves owners", "[mcp][session]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::ToolRegistry registry;
  const QString name = QStringLiteral("renderdoc_closeSession");
  registry.Add(MCP::ToolDefinition{name, name, QStringLiteral("Existing owner"),
      MCP::ToolEffects::ReadOnly, MCP::ToolRequirements::None, QJsonObject()},
      [](const QJsonObject &, MCP::CallContext &) {
        return MCP::ToolResult::Success(QJsonObject{{QStringLiteral("original"), true}});
      });
  MCP::RegisterSessionTools(registry, adapter);
  REQUIRE(registry.Count() == 11);
  MCP::RegisterSessionTools(registry, adapter);
  REQUIRE(registry.Count() == 11);
  MCP::CallContext call;
  REQUIRE(registry.Call(name, QJsonObject(), call).structured.value(QStringLiteral("original")).toBool());
  for(const MCP::ToolDefinition &def : registry.Definitions())
    REQUIRE(registry.Find(def.name) != NULL);
}

TEST_CASE("Native MCP launch requires explicit platform and validates contracts", "[mcp][session]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::ToolRegistry registry;
  MCP::RegisterSessionTools(registry, adapter);
  MCP::CallContext call;
  MCP::ToolResult result = registry.Call(QStringLiteral("renderdoc_launchApplication"),
      QJsonObject{{QStringLiteral("app"), QStringLiteral("not-launched")}}, call);
  REQUIRE(result.ok);
  REQUIRE_FALSE(result.structured.value(QStringLiteral("ok")).toBool());
  REQUIRE(result.structured.value(QStringLiteral("code")).toString() == QStringLiteral("PLATFORM_REQUIRED"));
  REQUIRE(adapter.ApplicationSessionState().value(QStringLiteral("phase")).toString() == QStringLiteral("idle"));
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_launchWindowsApplication"), QJsonObject(), call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_launchRemoteApplication"), QJsonObject(), call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_captureFrame"),
      QJsonObject{{QStringLiteral("frameNumber"), -1}}, call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_captureFrame"),
      QJsonObject{{QStringLiteral("trigger"), QStringLiteral("unknown")}}, call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_waitForCapture"),
      QJsonObject{{QStringLiteral("pollMs"), 1}}, call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_waitForLiveTarget"),
      QJsonObject{{QStringLiteral("timeoutMs"), 60001}}, call).ok);
  REQUIRE_FALSE(registry.Call(QStringLiteral("renderdoc_openCapture"), QJsonObject(), call).ok);
}

TEST_CASE("Native MCP close is idempotent and preserves local capture files", "[mcp][session]")
{
  QTemporaryDir directory;
  REQUIRE(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("existing.rdc"));
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly));
  REQUIRE(file.write("existing capture bytes") > 0);
  file.close();
  MCP::QueryAdapter adapter(NULL);
  MCP::CallContext call;
  for(int i = 0; i < 2; i++)
  {
    MCP::ToolResult result = adapter.SessionTool(QStringLiteral("renderdoc_closeSession"), QJsonObject(), call);
    REQUIRE(result.ok);
    REQUIRE(result.structured.value(QStringLiteral("closed")).toBool());
    REQUIRE_FALSE(result.structured.value(QStringLiteral("capturesDeleted")).toBool());
    REQUIRE_FALSE(result.structured.value(QStringLiteral("applicationTerminated")).toBool());
    REQUIRE(QFile::exists(path));
  }
  REQUIRE(file.open(QIODevice::ReadOnly));
  REQUIRE(file.readAll() == QByteArray("existing capture bytes"));
}

TEST_CASE("Native MCP waits and interruption do not launch applications", "[mcp][session]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::CallContext call;
  for(const QString &name : {QStringLiteral("renderdoc_waitForLiveTarget"), QStringLiteral("renderdoc_waitForCapture")})
  {
    MCP::ToolResult result = adapter.SessionTool(name,
        QJsonObject{{QStringLiteral("timeoutMs"), 100}, {QStringLiteral("pollMs"), 50}}, call);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.structured.value(QStringLiteral("ok")).toBool());
    REQUIRE(result.structured.value(QStringLiteral("timedOut")).toBool());
  }
  call.cancellationState.reset(new QAtomicInt(1));
  REQUIRE_FALSE(adapter.SessionTool(QStringLiteral("renderdoc_closeSession"), QJsonObject(), call).ok);
  REQUIRE(adapter.ApplicationSessionState().value(QStringLiteral("phase")).toString() == QStringLiteral("idle"));
  call.cancellationState.clear();
  call.deadlineMs = QDateTime::currentMSecsSinceEpoch() - 1;
  REQUIRE_FALSE(adapter.SessionTool(QStringLiteral("renderdoc_launchWindowsApplication"),
      QJsonObject{{QStringLiteral("executablePath"), QStringLiteral("invalid.exe")}}, call).ok);
  REQUIRE(adapter.ApplicationSessionState().value(QStringLiteral("phase")).toString() == QStringLiteral("idle"));
  call.deadlineMs = 0;
  REQUIRE_FALSE(adapter.SessionTool(QStringLiteral("renderdoc_unknownSessionTool"), QJsonObject(), call).ok);
}
TEST_CASE("Native MCP combined catalog adds eleven names and exposes application state", "[mcp][session]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::ToolRegistry registry;
  MCP::RegisterCoreTools(registry, adapter);
  MCP::RegisterExternalToolTools(registry, adapter);
  const int before = registry.Count();
  REQUIRE(before == 48);
  MCP::RegisterSessionTools(registry, adapter);
  REQUIRE(registry.Count() == before + 11);
  MCP::CallContext call;
  MCP::ToolResult result = registry.Call(QStringLiteral("renderdoc_getSessionState"), QJsonObject(), call);
  REQUIRE(result.ok);
  REQUIRE(result.structured.value(QStringLiteral("capture")).isObject());
  REQUIRE(result.structured.value(QStringLiteral("applicationSession")).toObject().value(QStringLiteral("phase")).toString() == QStringLiteral("idle"));
}

TEST_CASE("Native MCP wait observes owned cancellation during execution", "[mcp][session]")
{
  MCP::QueryAdapter adapter(NULL);
  MCP::CallContext call;
  call.cancellationState.reset(new QAtomicInt(0));
  const QSharedPointer<QAtomicInt> cancel = call.cancellationState;
  std::thread interrupter([cancel]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancel->storeRelease(1);
  });
  MCP::ToolResult result = adapter.SessionTool(QStringLiteral("renderdoc_waitForCapture"),
      QJsonObject{{QStringLiteral("timeoutMs"), 1000}, {QStringLiteral("pollMs"), 50}}, call);
  interrupter.join();
  REQUIRE_FALSE(result.ok);
  REQUIRE(int(result.error.code) == int(MCP::ErrorCode::Cancelled));
  REQUIRE(adapter.ApplicationSessionState().value(QStringLiteral("phase")).toString() == QStringLiteral("idle"));
}
#endif

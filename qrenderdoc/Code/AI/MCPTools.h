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

#pragma once

#include <QString>
#include "MCPToolRegistry.h"
#include "MCPQueryAdapter.h"

namespace MCP
{

// P0-7 Core read-only MCP tools
// -----------------------------
// This is where the tool *contract* lives: each tool declares its name, title,
// description, effects, requirements and JSON input schema, plus the handler that
// turns validated arguments into a ToolResult. Nothing here talks to the capture
// directly - every handler goes through a QueryAdapter, so all capture access
// stays in one place (MCPQueryAdapter) and is automatically marshalled onto the
// correct thread (UI thread for snapshot reads, replay thread for
// IReplayController calls).
//
// The tools registered here are deliberately read-only: they never mutate capture
// state, never touch the filesystem and never launch processes. Their
// ToolRequirements describe what must be loaded before they will run.
//
// Enum formatting is delegated to the name helpers declared below. They are
// implemented in MCPQueryAdapter.cpp, the single translation unit that includes
// renderdoc_tostr.inl, so tool code never re-includes the ToStr<>
// specialisations (which are non-inline and would cause duplicate symbols).

namespace detail
{
// Enum -> display name. See the note above for why these are not inlined here.
QString ShaderStageName(ShaderStage s);
QString TopologyName(Topology t);
QString FillModeName(FillMode m);
QString CullModeName(CullMode m);
QString CompareFunctionName(CompareFunction f);
QString StencilOperationName(StencilOperation o);
QString LogicOperationName(LogicOperation o);
QString BlendMultiplierName(BlendMultiplier b);
QString BlendOperationName(BlendOperation o);
QString FilterModeName(FilterMode f);
QString AddressModeName(AddressMode a);
QString VarTypeName(VarType t);
}    // namespace detail

// Legacy resource reference shape, shared by resource-based query tools.
QJsonObject ResourceRef(ResourceId id);

// Registers every core read-only tool into `registry`. `adapter` must outlive the
// registry's use (both live for the lifetime of the MCP server). Registration is
// idempotent per process because it happens once at server startup.
//
// Returns the number of tools registered.
int RegisterCoreTools(ToolRegistry &registry, QueryAdapter &adapter);

}    // namespace MCP

/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2026 Baldur Karlsson
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

#include "qc_counters.h"

// Non-Android stub implementation, mirroring arm_counters_stub.cpp. Without this
// the non-Android builds would fail to link once the backend is referenced from
// the API replay paths.

QCCounters::QCCounters()
{
}

QCCounters::~QCCounters()
{
}

bool QCCounters::Init(ApiType api, void *context, const char *renderer)
{
  return false;
}

rdcarray<GPUCounter> QCCounters::GetPublicCounterIds()
{
  return {};
}

CounterDescription QCCounters::GetCounterDescription(GPUCounter index)
{
  return {};
}

void QCCounters::EnableCounter(GPUCounter counter)
{
}

void QCCounters::DisableAllCounters()
{
}

uint32_t QCCounters::GetPassCount()
{
  return 0;
}

void QCCounters::BeginPass(uint32_t passID)
{
}

void QCCounters::EndPass()
{
}

void QCCounters::BeginSample(uint32_t eventId)
{
}

void QCCounters::EndSample()
{
}

rdcarray<CounterResult> QCCounters::GetCounterData(const rdcarray<uint32_t> &eventIDs,
                                                   const rdcarray<GPUCounter> &counters)
{
  return {};
}

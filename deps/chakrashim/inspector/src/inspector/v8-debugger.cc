// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/inspector/v8-debugger.h"

#include "src/inspector/protocol/Protocol.h"
#include "src/inspector/script-breakpoint.h"
#include "src/inspector/string-util.h"
#include "src/inspector/v8-debugger-agent-impl.h"
#include "src/inspector/v8-inspector-impl.h"
#include "src/inspector/v8-stack-trace-impl.h"

#include "src/jsrtinspector.h"
#include "src/jsrtinspectorhelpers.h"
#include <assert.h>

namespace v8_inspector {

V8Debugger::V8Debugger(v8::Isolate* isolate, V8InspectorImpl* inspector)
    : m_isolate(isolate),
      m_inspector(inspector),
      m_lastContextId(0),
      m_enableCount(0),
      m_breakpointsActivated(true),
      m_runningNestedMessageLoop(false),
      m_ignoreScriptParsedEventsCounter(0),
      m_maxAsyncCallStackDepth(0),
      m_pauseOnNextStatement(false) {}

V8Debugger::~V8Debugger() {}

void V8Debugger::enable() {
  if (m_enableCount++) return;

  jsrt::Inspector::SetDebugEventHandler(V8Debugger::JsDiagDebugEventHandler, this);
  m_debuggerContext.Reset(m_isolate, v8::Debug::GetDebugContext(m_isolate));
}

void V8Debugger::disable() {
  if (--m_enableCount) return;
  ClearBreakpoints();
  m_debuggerContext.Reset();
  jsrt::Inspector::SetDebugEventHandler(nullptr, nullptr);
}

bool V8Debugger::enabled() const { return m_enableCount > 0; }

// static
int V8Debugger::contextId(v8::Local<v8::Context> context) {
  v8::Local<v8::Value> data =
      context->GetEmbedderData(static_cast<int>(v8::Context::kDebugIdIndex));
  if (data.IsEmpty() || !data->IsString()) return 0;
  String16 dataString = toProtocolString(data.As<v8::String>());
  if (dataString.isEmpty()) return 0;
  size_t commaPos = dataString.find(",");
  if (commaPos == String16::kNotFound) return 0;
  size_t commaPos2 = dataString.find(",", commaPos + 1);
  if (commaPos2 == String16::kNotFound) return 0;
  return dataString.substring(commaPos + 1, commaPos2 - commaPos - 1)
      .toInteger();
}

// static
int V8Debugger::getGroupId(v8::Local<v8::Context> context) {
  v8::Local<v8::Value> data =
      context->GetEmbedderData(static_cast<int>(v8::Context::kDebugIdIndex));
  if (data.IsEmpty() || !data->IsString()) return 0;
  String16 dataString = toProtocolString(data.As<v8::String>());
  if (dataString.isEmpty()) return 0;
  size_t commaPos = dataString.find(",");
  if (commaPos == String16::kNotFound) return 0;
  return dataString.substring(0, commaPos).toInteger();
}

void V8Debugger::getCompiledScripts(
    int contextGroupId,
    std::vector<std::unique_ptr<V8DebuggerScript>>& result) {
  JsValueRef scripts;
  JsDiagGetScripts(&scripts);

  int length;
  if (jsrt::InspectorHelpers::GetIntProperty(scripts, "length", &length) != JsNoError) {
    assert(false);
    return;
  }

  for (int i = 0; i < length; i++) {
    JsValueRef index;
    if (JsIntToNumber(i, &index) != JsNoError) {
      assert(false);
      return;
    }

    JsValueRef script;
    if (JsGetIndexedProperty(scripts, index, &script) != JsNoError) {
      assert(false);
      return;
    }

    result.push_back(wrapUnique(new V8DebuggerScript(m_isolate, script, false)));
  }
}

String16 V8Debugger::setBreakpoint(const String16& sourceID,
                                   const ScriptBreakpoint& scriptBreakpoint,
                                   int* actualLineNumber,
                                   int* actualColumnNumber) {
  *actualLineNumber = 0;
  *actualColumnNumber = 0;

  bool ok = false;
  int srcId = sourceID.toInteger(&ok);
  if (!ok) {
    return String16();
  }

  JsValueRef breakpoint;
  if (JsDiagSetBreakpoint(srcId, scriptBreakpoint.lineNumber, scriptBreakpoint.columnNumber, &breakpoint) != JsNoError) {
    return String16();
  }

  int breakpointId;
  if (jsrt::InspectorHelpers::GetIntProperty(breakpoint, "breakpointId",
                                             &breakpointId) != JsNoError) {
    return String16();
  }

  if (jsrt::InspectorHelpers::GetIntProperty(breakpoint, "line",
                                             actualLineNumber) != JsNoError) {
    return String16();
  }

  if (jsrt::InspectorHelpers::GetIntProperty(breakpoint, "column",
                                             actualColumnNumber) != JsNoError) {
    return String16();
  }

  return String16::fromInteger(breakpointId);
}

void V8Debugger::removeBreakpoint(const String16& breakpointId) {
  bool ok = false;
  int bpId = breakpointId.toInteger(&ok);
  if (!ok) {
    assert(false);
    return;
  }

  JsErrorCode err = JsDiagRemoveBreakpoint(bpId);
  assert(err == JsNoError || err == JsErrorInvalidArgument);
}

void V8Debugger::setBreakpointsActivated(bool activated) {
  
}

V8Debugger::PauseOnExceptionsState V8Debugger::getPauseOnExceptionsState() {
  PauseOnExceptionsState state = PauseOnExceptionsState::DontPauseOnExceptions;
  JsDiagBreakOnExceptionAttributes breakAttr;
  if (JsDiagGetBreakOnException(jsrt::InspectorHelpers::GetRuntimeFromIsolate(m_isolate), &breakAttr) != JsNoError) {
    assert(false);
    return state;
  }

  switch (breakAttr) {
  case JsDiagBreakOnExceptionAttributeFirstChance:
    state = PauseOnExceptionsState::PauseOnAllExceptions;
    break;

  case JsDiagBreakOnExceptionAttributeUncaught:
    state = PauseOnExceptionsState::PauseOnUncaughtExceptions;
    break;

  case JsDiagBreakOnExceptionAttributeNone:
    state = PauseOnExceptionsState::DontPauseOnExceptions;
    break;

  default:
    // This should never happen unless new values are added.
    assert(false);
  }

  return state;
}

void V8Debugger::setPauseOnExceptionsState(
    PauseOnExceptionsState pauseOnExceptionsState) {
  JsDiagBreakOnExceptionAttributes breakAttr = JsDiagBreakOnExceptionAttributeNone;

  switch (pauseOnExceptionsState) {
  case PauseOnExceptionsState::PauseOnAllExceptions:
    breakAttr = JsDiagBreakOnExceptionAttributeFirstChance;
    break;

  case PauseOnExceptionsState::PauseOnUncaughtExceptions:
    breakAttr = JsDiagBreakOnExceptionAttributeUncaught;
    break;

  case PauseOnExceptionsState::DontPauseOnExceptions:
    breakAttr = JsDiagBreakOnExceptionAttributeNone;
    break;

  default:
    // This should never happen unless new values are added.
    assert(false);
  }

  if (JsDiagSetBreakOnException(jsrt::InspectorHelpers::GetRuntimeFromIsolate(m_isolate), breakAttr) != JsNoError) {
    assert(false);
  }
}

void V8Debugger::setPauseOnNextStatement(bool pause) {
  if (m_runningNestedMessageLoop) return;
  if (pause) {
    m_pauseOnNextStatement = true;
    jsrt::Inspector::RequestAsyncBreak(m_isolate);
  } else {
    m_pauseOnNextStatement = false;
  }
}

bool V8Debugger::canBreakProgram() {
  if (!m_breakpointsActivated) return false;
  return m_isolate->InContext();
}

void V8Debugger::breakProgram() {
}

void V8Debugger::continueProgram() {
  if (isPaused()) m_inspector->client()->quitMessageLoopOnPause();
  m_pausedContext.Clear();
}

void V8Debugger::stepIntoStatement() {
  DCHECK(isPaused());
  JsDiagSetStepType(JsDiagStepTypeStepIn);
  continueProgram();
}

void V8Debugger::stepOverStatement() {
  DCHECK(isPaused());
  JsDiagSetStepType(JsDiagStepTypeStepOver);
  continueProgram();
}

void V8Debugger::stepOutOfFunction() {
  DCHECK(isPaused());
  JsDiagSetStepType(JsDiagStepTypeStepOut);
  continueProgram();
}

void V8Debugger::clearStepping() {
  DCHECK(enabled());
  JsDiagSetStepType(JsDiagStepTypeContinue);
}

bool V8Debugger::setScriptSource(
    const String16& sourceID, v8::Local<v8::String> newSource, bool dryRun,
    ErrorString* error,
    Maybe<protocol::Runtime::ExceptionDetails>* exceptionDetails,
    JavaScriptCallFrames* newCallFrames, Maybe<bool>* stackChanged) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);

  return false;
}

JavaScriptCallFrames V8Debugger::currentCallFrames(int limit) {
  if (!m_isolate->InContext()) return JavaScriptCallFrames();
  
  JsValueRef stackTrace;
  JsDiagGetStackTrace(&stackTrace);

  int length;
  if (jsrt::InspectorHelpers::GetIntProperty(stackTrace, "length", &length)
      != JsNoError) {
    assert(false);
    return JavaScriptCallFrames();
  }

  if (limit > 0 && limit < length) {
    length = limit;
  }
  
  JavaScriptCallFrames callFrames;
  for (int i = 0; i < length; ++i) {
    JsValueRef callFrameValue;
    if (jsrt::InspectorHelpers::GetIndexedProperty(stackTrace, i, &callFrameValue) != JsNoError) {
      assert(false);
      return JavaScriptCallFrames();
    }

    callFrames.push_back(JavaScriptCallFrame::create(
        debuggerContext(), callFrameValue));
  }
  return callFrames;
}

V8StackTraceImpl* V8Debugger::currentAsyncCallChain() {
  // CHAKRA-TODO - Figure out what to do here
  return nullptr;
}

v8::MaybeLocal<v8::Array> V8Debugger::internalProperties(
    v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);

  return v8::MaybeLocal<v8::Array>();
}

bool V8Debugger::isPaused() {
  return !m_pausedContext.IsEmpty();
}

std::unique_ptr<V8StackTraceImpl> V8Debugger::createStackTrace(
    v8::Local<v8::StackTrace> stackTrace) {
  // CHAKRA-TODO - Figure out what to do here
  return std::unique_ptr<V8StackTraceImpl>(nullptr);
}

int V8Debugger::markContext(const V8ContextInfo& info) {
  DCHECK(info.context->GetIsolate() == m_isolate);
  int contextId = ++m_lastContextId;
  String16 debugData = String16::fromInteger(info.contextGroupId) + "," +
                       String16::fromInteger(contextId) + "," +
                       toString16(info.auxData);
  v8::Context::Scope contextScope(info.context);
  info.context->SetEmbedderData(static_cast<int>(v8::Context::kDebugIdIndex),
                                toV8String(m_isolate, debugData));
  return contextId;
}

void V8Debugger::setAsyncCallStackDepth(V8DebuggerAgentImpl* agent, int depth) {
  if (depth <= 0)
    m_maxAsyncCallStackDepthMap.erase(agent);
  else
    m_maxAsyncCallStackDepthMap[agent] = depth;

  int maxAsyncCallStackDepth = 0;
  for (const auto& pair : m_maxAsyncCallStackDepthMap) {
    if (pair.second > maxAsyncCallStackDepth)
      maxAsyncCallStackDepth = pair.second;
  }

  if (m_maxAsyncCallStackDepth == maxAsyncCallStackDepth) return;
  m_maxAsyncCallStackDepth = maxAsyncCallStackDepth;
  if (!maxAsyncCallStackDepth) allAsyncTasksCanceled();
}

void V8Debugger::asyncTaskScheduled(const StringView& taskName, void* task,
                                    bool recurring) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

void V8Debugger::asyncTaskScheduled(const String16& taskName, void* task,
                                    bool recurring) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

void V8Debugger::asyncTaskCanceled(void* task) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

void V8Debugger::asyncTaskStarted(void* task) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

void V8Debugger::asyncTaskFinished(void* task) {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

void V8Debugger::allAsyncTasksCanceled() {
  // CHAKRA-TODO - Figure out what to do here
  ////assert(false);
}

void V8Debugger::muteScriptParsedEvents() {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

void V8Debugger::unmuteScriptParsedEvents() {
  // CHAKRA-TODO - Figure out what to do here
  assert(false);
}

std::unique_ptr<V8StackTraceImpl> V8Debugger::captureStackTrace(
    bool fullStack) {
  // CHAKRA-TODO - Figure out what to do here
  ////assert(false);

  return std::unique_ptr<V8StackTraceImpl>(nullptr);
}

void V8Debugger::JsDiagDebugEventHandler(
    JsDiagDebugEvent debugEvent,
    JsValueRef eventData,
    void* callbackState) {
  if (callbackState != nullptr) {
    static_cast<V8Debugger*>(callbackState)->DebugEventHandler(debugEvent, eventData);
  }
}

v8::Local<v8::Context> V8Debugger::debuggerContext() const {
  DCHECK(!m_debuggerContext.IsEmpty());
  return m_debuggerContext.Get(m_isolate);
}

void V8Debugger::DebugEventHandler(
    JsDiagDebugEvent debugEvent,
    JsValueRef eventData) {
  switch (debugEvent) {
  case JsDiagDebugEventSourceCompile:
  case JsDiagDebugEventCompileError:
    HandleSourceEvents(eventData, debugEvent == JsDiagDebugEventSourceCompile);
    break;

  case JsDiagDebugEventBreakpoint:
  case JsDiagDebugEventStepComplete:
  case JsDiagDebugEventDebuggerStatement:
  case JsDiagDebugEventRuntimeException:
    HandleBreak(eventData);
    break;

  case JsDiagDebugEventAsyncBreak:
    if (m_pauseOnNextStatement) {
      m_pauseOnNextStatement = false;
      HandleBreak(eventData);
    }

    break;
  }
}

void V8Debugger::HandleSourceEvents(JsValueRef eventData, bool success) {
  V8DebuggerAgentImpl *agent = m_inspector->enabledDebuggerAgentForGroup(
      getGroupId(m_isolate->GetCurrentContext()));

  if (agent != nullptr)
  {
    agent->didParseSource(wrapUnique(new V8DebuggerScript(m_isolate, eventData, false)),
                          success);
  }
}

void V8Debugger::HandleBreak(JsValueRef eventData) {
  // Don't allow nested breaks.
  if (m_runningNestedMessageLoop) return;

  v8::Local<v8::Context> pausedContext = m_isolate->GetCurrentContext();
  v8::Local<v8::Object> executionState;
  v8::Local<v8::Value> exception = jsrt::InspectorHelpers::WrapRuntimeException(
      eventData);

  V8DebuggerAgentImpl* agent =
      m_inspector->enabledDebuggerAgentForGroup(getGroupId(pausedContext));
  if (!agent) return;

  std::vector<String16> breakpointIds;

  bool hasBreakpointId = false;
  if (jsrt::InspectorHelpers::HasProperty(eventData, "breakpointId",
                                          &hasBreakpointId) != JsNoError) {
    assert(false);
    return;
  }

  if (hasBreakpointId) {
    breakpointIds.reserve(1);

    int breakpointId;
    if (jsrt::InspectorHelpers::GetIntProperty(eventData, "breakpointId",
                                               &breakpointId) == JsNoError) {
      breakpointIds.push_back(String16::fromInteger(breakpointId));
    }
    else {
      assert(false);
    }
  }

  bool hasUncaught = false;
  if (jsrt::InspectorHelpers::HasProperty(eventData, "uncaught", &hasUncaught)
      != JsNoError) {
    assert(false);
    return;
  }

  bool isUncaught = false;
  if (hasUncaught) {
    if (jsrt::InspectorHelpers::GetBoolProperty(eventData, "uncaught",
                                                &isUncaught) != JsNoError) {

    }
  }

  m_pausedContext = pausedContext;
  V8DebuggerAgentImpl::SkipPauseRequest result = agent->didPause(
      pausedContext, exception, breakpointIds, false, isUncaught);
  if (result == V8DebuggerAgentImpl::RequestNoSkip) {
    m_runningNestedMessageLoop = true;
    int groupId = getGroupId(pausedContext);
    DCHECK(groupId);
    m_inspector->client()->runMessageLoopOnPause(groupId);
    // The agent may have been removed in the nested loop.
    agent =
        m_inspector->enabledDebuggerAgentForGroup(getGroupId(pausedContext));
    if (agent) agent->didContinue();
    m_runningNestedMessageLoop = false;
  }
  m_pausedContext.Clear();

  if (result == V8DebuggerAgentImpl::RequestStepFrame ||
      result == V8DebuggerAgentImpl::RequestStepInto) {
    JsDiagSetStepType(JsDiagStepTypeStepIn);
  } else if (result == V8DebuggerAgentImpl::RequestStepOut) {
    JsDiagSetStepType(JsDiagStepTypeStepOut);
  }
}

void V8Debugger::ClearBreakpoints() {
  jsrt::Inspector::ClearBreakpoints();
}

}  // namespace v8_inspector

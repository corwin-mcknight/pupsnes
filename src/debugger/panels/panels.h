#pragma once

namespace pupsnes::debugger {

class DebuggerApp;

void RenderControlsPanel(DebuggerApp& app);
void RenderRegistersPanel(DebuggerApp& app);
void RenderDisasmPanel(DebuggerApp& app);
void RenderMemoryPanel(DebuggerApp& app);
void RenderTracePanel(DebuggerApp& app);
void RenderSchedulerPanel(DebuggerApp& app);
void RenderErrorsPanel(DebuggerApp& app);

}  // namespace pupsnes::debugger

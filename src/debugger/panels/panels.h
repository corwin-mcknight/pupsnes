#pragma once

namespace pupsnes::debugger {

class DebuggerApp;

void RenderControlsPanel(DebuggerApp& app);
void RenderRegistersPanel(DebuggerApp& app);
void RenderDisasmPanel(DebuggerApp& app);
void RenderMemoryPanel(DebuggerApp& app);
void RenderStackPanel(DebuggerApp& app);
void RenderPpuPanel(DebuggerApp& app);
void RenderTracePanel(DebuggerApp& app);
void RenderTraceRecordPanel(DebuggerApp& app);
void RenderMicroOpTracePanel(DebuggerApp& app);
void RenderSchedulerPanel(DebuggerApp& app);
void RenderErrorsPanel(DebuggerApp& app);
void RenderBusEventPanel(DebuggerApp& app);
void RenderLoadRomDialog(DebuggerApp& app);
void InitFileShortcuts(DebuggerApp& app);

}  // namespace pupsnes::debugger

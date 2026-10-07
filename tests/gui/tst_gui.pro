QT += widgets svg testlib concurrent core-private
CONFIG += no_private_qt_headers_warning

CONFIG += console c++17 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_gui

# ../.. = sumber aplikasi, .. = FakeAgentRuntime.h
INCLUDEPATH += ../.. ..

# Windows Credential Manager (SecretStore.cpp)
win32: LIBS += -ladvapi32

# Semua sumber aplikasi kecuali main.cpp: MainWindow dirakit sendiri oleh test
SOURCES += \
    tst_gui.cpp \
    ../../AgentAccess.cpp \
    ../../AppFonts.cpp \
    ../../BranchViewer.cpp \
    ../../CanvasAutomation.cpp \
    ../../CanvasBoard.cpp \
    ../../CanvasChat.cpp \
    ../../CanvasChatPanel.cpp \
    ../../CanvasInspector.cpp \
    ../../CanvasItems.cpp \
    ../../CanvasLibrary.cpp \
    ../../CanvasModel.cpp \
    ../../CanvasPage.cpp \
    ../../CanvasPreviewDrawer.cpp \
    ../../CanvasView.cpp \
    ../../CanvasWorkflow.cpp \
    ../../CanvasWorkspace.cpp \
    ../../ClaudeCli.cpp \
    ../../CSharpMetrics.cpp \
    ../../ClassDiagram.cpp \
    ../../ClaudeCodeLogin.cpp \
    ../../ClaudeCodeRuntime.cpp \
    ../../CodeMetrics.cpp \
    ../../ConsolePanelWidget.cpp \
    ../../ConsoleTaskCard.cpp \
    ../../ElidedLabel.cpp \
    ../../DiagramViewer.cpp \
    ../../DiffView.cpp \
    ../../DocumentText.cpp \
    ../../FileManager.cpp \
    ../../FolderLauncher.cpp \
    ../../FontPickerDialog.cpp \
    ../../GitHistory.cpp \
    ../../GitProcess.cpp \
    ../../HoverInfoPopup.cpp \
    ../../IntegrationsDialog.cpp \
    ../../KanbanCardWidget.cpp \
    ../../KanbanColumnWidget.cpp \
    ../../MaintainabilityView.cpp \
    ../../MarkdownView.cpp \
    ../../MermaidRenderer.cpp \
    ../../NewTaskDialog.cpp \
    ../../PromptComposer.cpp \
    ../../PromptEditor.cpp \
    ../../ResponseDrawer.cpp \
    ../../RunLogFormatter.cpp \
    ../../RuntimeNoticeDialog.cpp \
    ../../RunPulse.cpp \
    ../../SecretStore.cpp \
    ../../SidePanelDock.cpp \
    ../../SourceTokens.cpp \
    ../../SplitterPaneAnimator.cpp \
    ../../StageCatalog.cpp \
    ../../StageInfo.cpp \
    ../../StageProfile.cpp \
    ../../StageSwarm.cpp \
    ../../StreamJsonParser.cpp \
    ../../SwarmCoordinator.cpp \
    ../../SwimlaneWidget.cpp \
    ../../TaskAttachments.cpp \
    ../../TaskGit.cpp \
    ../../TaskManager.cpp \
    ../../Theme.cpp \
    ../../TransitionPolicy.cpp \
    ../../VerticalTabButton.cpp \
    ../../WorkspaceDiff.cpp \
    ../../WorkspaceGuard.cpp \
    ../../mainwindow.cpp

HEADERS += \
    ../FakeAgentRuntime.h \
    ../GitSandbox.h \
    ../../AgentAccess.h \
    ../../AgentDefinition.h \
    ../../AgentRuntime.h \
    ../../AgentTypes.h \
    ../../AppFonts.h \
    ../../AppSettings.h \
    ../../BranchViewer.h \
    ../../CanvasAutomation.h \
    ../../CanvasBoard.h \
    ../../CanvasChat.h \
    ../../CanvasChatPanel.h \
    ../../CanvasInspector.h \
    ../../CanvasItems.h \
    ../../CanvasLibrary.h \
    ../../CanvasModel.h \
    ../../CanvasPage.h \
    ../../CanvasPreviewDrawer.h \
    ../../CanvasView.h \
    ../../CanvasWorkflow.h \
    ../../CanvasWorkspace.h \
    ../../ClaudeCli.h \
    ../../CSharpMetrics.h \
    ../../ClassDiagram.h \
    ../../ClaudeCodeLogin.h \
    ../../ClaudeCodeRuntime.h \
    ../../CodeMetrics.h \
    ../../ConsolePanelWidget.h \
    ../../ConsoleTaskCard.h \
    ../../ElidedLabel.h \
    ../../DiagramViewer.h \
    ../../DiffView.h \
    ../../DocumentText.h \
    ../../FileManager.h \
    ../../FolderLauncher.h \
    ../../FontPickerDialog.h \
    ../../GitHistory.h \
    ../../GitProcess.h \
    ../../HoverInfoPopup.h \
    ../../IntegrationsDialog.h \
    ../../KanbanCardWidget.h \
    ../../KanbanColumnWidget.h \
    ../../MaintainabilityView.h \
    ../../MarkdownView.h \
    ../../MermaidRenderer.h \
    ../../NewTaskDialog.h \
    ../../PromptComposer.h \
    ../../PromptEditor.h \
    ../../ResponseDrawer.h \
    ../../RunLogFormatter.h \
    ../../RuntimeNoticeDialog.h \
    ../../RunPulse.h \
    ../../SecretStore.h \
    ../../SidePanelDock.h \
    ../../SourceTokens.h \
    ../../SplitterPaneAnimator.h \
    ../../StageCatalog.h \
    ../../StageInfo.h \
    ../../StageProfile.h \
    ../../StageSwarm.h \
    ../../StreamJsonParser.h \
    ../../SwarmCoordinator.h \
    ../../SwimlaneWidget.h \
    ../../TaskAttachments.h \
    ../../TaskGit.h \
    ../../TaskItem.h \
    ../../TaskManager.h \
    ../../Theme.h \
    ../../TaskMaterials.h \
    ../../TransitionPolicy.h \
    ../../VerticalTabButton.h \
    ../../WorkspaceDiff.h \
    ../../WorkspaceGuard.h \
    ../../mainwindow.h

FORMS += \
    ../../ConsolePanelWidget.ui \
    ../../KanbanCardWidget.ui \
    ../../KanbanColumnWidget.ui \
    ../../SwimlaneWidget.ui \
    ../../mainwindow.ui

RESOURCES += ../../Resources/resources.qrc

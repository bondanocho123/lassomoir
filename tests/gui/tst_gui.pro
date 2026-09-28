QT += widgets svg testlib concurrent core-private
CONFIG += no_private_qt_headers_warning

CONFIG += console c++17 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_gui

# ../.. = sumber aplikasi, .. = FakeAgentRuntime.h
INCLUDEPATH += ../.. ..

# Semua sumber aplikasi kecuali main.cpp: MainWindow dirakit sendiri oleh test
SOURCES += \
    tst_gui.cpp \
    ../../ClaudeCli.cpp \
    ../../CSharpMetrics.cpp \
    ../../ClassDiagram.cpp \
    ../../ClaudeCodeRuntime.cpp \
    ../../CodeMetrics.cpp \
    ../../ConsolePanelWidget.cpp \
    ../../DiffView.cpp \
    ../../DocumentText.cpp \
    ../../FileManager.cpp \
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
    ../../SourceTokens.cpp \
    ../../SplitterPaneAnimator.cpp \
    ../../StageCatalog.cpp \
    ../../StageProfile.cpp \
    ../../StageSwarm.cpp \
    ../../StreamJsonParser.cpp \
    ../../SwarmCoordinator.cpp \
    ../../SwimlaneWidget.cpp \
    ../../TaskAttachments.cpp \
    ../../TaskManager.cpp \
    ../../TransitionPolicy.cpp \
    ../../WorkspaceDiff.cpp \
    ../../WorkspaceGuard.cpp \
    ../../mainwindow.cpp

HEADERS += \
    ../FakeAgentRuntime.h \
    ../../AgentDefinition.h \
    ../../AgentRuntime.h \
    ../../AgentTypes.h \
    ../../ClaudeCli.h \
    ../../CSharpMetrics.h \
    ../../ClassDiagram.h \
    ../../ClaudeCodeRuntime.h \
    ../../CodeMetrics.h \
    ../../ConsolePanelWidget.h \
    ../../DiffView.h \
    ../../DocumentText.h \
    ../../FileManager.h \
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
    ../../SourceTokens.h \
    ../../SplitterPaneAnimator.h \
    ../../StageCatalog.h \
    ../../StageProfile.h \
    ../../StageSwarm.h \
    ../../StreamJsonParser.h \
    ../../SwarmCoordinator.h \
    ../../SwimlaneWidget.h \
    ../../TaskAttachments.h \
    ../../TaskItem.h \
    ../../TaskManager.h \
    ../../TaskMaterials.h \
    ../../TransitionPolicy.h \
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

QT += widgets svg testlib

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
    ../../ClaudeCodeRuntime.cpp \
    ../../ConsolePanelWidget.cpp \
    ../../FileManager.cpp \
    ../../KanbanCardWidget.cpp \
    ../../KanbanColumnWidget.cpp \
    ../../MarkdownView.cpp \
    ../../MermaidRenderer.cpp \
    ../../NewTaskDialog.cpp \
    ../../PromptComposer.cpp \
    ../../ResponseDrawer.cpp \
    ../../RunLogFormatter.cpp \
    ../../SplitterPaneAnimator.cpp \
    ../../StageCatalog.cpp \
    ../../StageProfile.cpp \
    ../../StageSwarm.cpp \
    ../../StreamJsonParser.cpp \
    ../../SwarmCoordinator.cpp \
    ../../SwimlaneWidget.cpp \
    ../../TaskManager.cpp \
    ../../TransitionPolicy.cpp \
    ../../WorkspaceGuard.cpp \
    ../../mainwindow.cpp

HEADERS += \
    ../FakeAgentRuntime.h \
    ../../AgentDefinition.h \
    ../../AgentRuntime.h \
    ../../AgentTypes.h \
    ../../ClaudeCli.h \
    ../../ClaudeCodeRuntime.h \
    ../../ConsolePanelWidget.h \
    ../../FileManager.h \
    ../../KanbanCardWidget.h \
    ../../KanbanColumnWidget.h \
    ../../MarkdownView.h \
    ../../MermaidRenderer.h \
    ../../NewTaskDialog.h \
    ../../PromptComposer.h \
    ../../ResponseDrawer.h \
    ../../RunLogFormatter.h \
    ../../SplitterPaneAnimator.h \
    ../../StageCatalog.h \
    ../../StageProfile.h \
    ../../StageSwarm.h \
    ../../StreamJsonParser.h \
    ../../SwarmCoordinator.h \
    ../../SwimlaneWidget.h \
    ../../TaskItem.h \
    ../../TaskManager.h \
    ../../TransitionPolicy.h \
    ../../WorkspaceGuard.h \
    ../../mainwindow.h

FORMS += \
    ../../ConsolePanelWidget.ui \
    ../../KanbanCardWidget.ui \
    ../../KanbanColumnWidget.ui \
    ../../SwimlaneWidget.ui \
    ../../mainwindow.ui

RESOURCES += ../../Resources/resources.qrc

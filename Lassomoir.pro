QT += widgets svg

CONFIG += c++17

# Icon file .exe di Windows (Explorer, taskbar)
win32: RC_ICONS = Resources/app.ico

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    ClaudeCli.cpp \
    ClaudeCodeRuntime.cpp \
    ConsolePanelWidget.cpp \
    EdgeMermaidRenderer.cpp \
    FileManager.cpp \
    KanbanCardWidget.cpp \
    KanbanColumnWidget.cpp \
    MarkdownView.cpp \
    MermaidRenderer.cpp \
    NewTaskDialog.cpp \
    PromptComposer.cpp \
    ResponseDrawer.cpp \
    RunLogFormatter.cpp \
    SplitterPaneAnimator.cpp \
    StageCatalog.cpp \
    StageProfile.cpp \
    StageSwarm.cpp \
    StreamJsonParser.cpp \
    SwarmCoordinator.cpp \
    SwimlaneWidget.cpp \
    TaskManager.cpp \
    TransitionPolicy.cpp \
    WorkspaceGuard.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    AgentDefinition.h \
    AgentRuntime.h \
    AgentTypes.h \
    ClaudeCli.h \
    ClaudeCodeRuntime.h \
    ConsolePanelWidget.h \
    EdgeMermaidRenderer.h \
    FileManager.h \
    KanbanCardWidget.h \
    KanbanColumnWidget.h \
    MarkdownView.h \
    MermaidRenderer.h \
    NewTaskDialog.h \
    PromptComposer.h \
    ResponseDrawer.h \
    RunLogFormatter.h \
    SplitterPaneAnimator.h \
    StageCatalog.h \
    StageProfile.h \
    StageSwarm.h \
    StreamJsonParser.h \
    SwarmCoordinator.h \
    SwimlaneWidget.h \
    TaskItem.h \
    TaskManager.h \
    TransitionPolicy.h \
    WorkspaceGuard.h \
    mainwindow.h

FORMS += \
    ConsolePanelWidget.ui \
    KanbanCardWidget.ui \
    KanbanColumnWidget.ui \
    SwimlaneWidget.ui \
    mainwindow.ui

RESOURCES += \
    Resources/resources.qrc

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

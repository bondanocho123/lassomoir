# core-private: QZipReader untuk membaca lampiran .xlsx/.docx (lihat DocumentText.cpp). API privat
# mengikat build ke versi Qt yang dipakai; peringatan qmake soal itu tidak perlu muncul tiap build.
QT += widgets svg concurrent core-private
CONFIG += no_private_qt_headers_warning

CONFIG += c++17

# Icon file .exe di Windows (Explorer, taskbar)
win32: RC_ICONS = Resources/app.ico

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    BranchViewer.cpp \
    ClaudeCli.cpp \
    CSharpMetrics.cpp \
    ClassDiagram.cpp \
    ClaudeCodeRuntime.cpp \
    CodeMetrics.cpp \
    ConsolePanelWidget.cpp \
    ConsoleTaskCard.cpp \
    ElidedLabel.cpp \
    DiagramViewer.cpp \
    DiffView.cpp \
    DocumentText.cpp \
    EdgeMermaidRenderer.cpp \
    FileManager.cpp \
    GitHistory.cpp \
    GitProcess.cpp \
    HoverInfoPopup.cpp \
    KanbanCardWidget.cpp \
    KanbanColumnWidget.cpp \
    MaintainabilityView.cpp \
    MarkdownView.cpp \
    MermaidRenderer.cpp \
    NewTaskDialog.cpp \
    PromptComposer.cpp \
    PromptEditor.cpp \
    ResponseDrawer.cpp \
    RunLogFormatter.cpp \
    RuntimeNoticeDialog.cpp \
    RunPulse.cpp \
    SourceTokens.cpp \
    SplitterPaneAnimator.cpp \
    StageCatalog.cpp \
    StageInfo.cpp \
    StageProfile.cpp \
    StageSwarm.cpp \
    StreamJsonParser.cpp \
    SwarmCoordinator.cpp \
    SwimlaneWidget.cpp \
    TaskAttachments.cpp \
    TaskGit.cpp \
    TaskManager.cpp \
    TransitionPolicy.cpp \
    WorkspaceDiff.cpp \
    WorkspaceGuard.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    AgentDefinition.h \
    AgentRuntime.h \
    AgentTypes.h \
    BranchViewer.h \
    ClaudeCli.h \
    CSharpMetrics.h \
    ClassDiagram.h \
    ClaudeCodeRuntime.h \
    CodeMetrics.h \
    ConsolePanelWidget.h \
    ConsoleTaskCard.h \
    ElidedLabel.h \
    DiagramViewer.h \
    DiffView.h \
    DocumentText.h \
    EdgeMermaidRenderer.h \
    FileManager.h \
    GitHistory.h \
    GitProcess.h \
    HoverInfoPopup.h \
    KanbanCardWidget.h \
    KanbanColumnWidget.h \
    MaintainabilityView.h \
    MarkdownView.h \
    MermaidRenderer.h \
    NewTaskDialog.h \
    PromptComposer.h \
    PromptEditor.h \
    ResponseDrawer.h \
    RunLogFormatter.h \
    RuntimeNoticeDialog.h \
    RunPulse.h \
    SourceTokens.h \
    SplitterPaneAnimator.h \
    StageCatalog.h \
    StageInfo.h \
    StageProfile.h \
    StageSwarm.h \
    StreamJsonParser.h \
    SwarmCoordinator.h \
    SwimlaneWidget.h \
    TaskAttachments.h \
    TaskGit.h \
    TaskItem.h \
    TaskManager.h \
    TaskMaterials.h \
    TransitionPolicy.h \
    WorkspaceDiff.h \
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

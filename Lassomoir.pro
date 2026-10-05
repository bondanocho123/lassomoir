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

# Windows Credential Manager (SecretStore.cpp)
win32: LIBS += -ladvapi32

SOURCES += \
    AgentAccess.cpp \
    AppFonts.cpp \
    BranchViewer.cpp \
    CanvasAutomation.cpp \
    CanvasBoard.cpp \
    CanvasInspector.cpp \
    CanvasItems.cpp \
    CanvasLibrary.cpp \
    CanvasModel.cpp \
    CanvasPage.cpp \
    CanvasView.cpp \
    CanvasWorkflow.cpp \
    CanvasWorkspace.cpp \
    ClaudeCli.cpp \
    CSharpMetrics.cpp \
    ClassDiagram.cpp \
    ClaudeCodeLogin.cpp \
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
    FolderLauncher.cpp \
    FontPickerDialog.cpp \
    GitHistory.cpp \
    GitProcess.cpp \
    HoverInfoPopup.cpp \
    IntegrationsDialog.cpp \
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
    SecretStore.cpp \
    SidePanelDock.cpp \
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
    Theme.cpp \
    TransitionPolicy.cpp \
    VerticalTabButton.cpp \
    WorkspaceDiff.cpp \
    WorkspaceGuard.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    AgentAccess.h \
    AgentDefinition.h \
    AgentRuntime.h \
    AgentTypes.h \
    AppFonts.h \
    AppSettings.h \
    BranchViewer.h \
    CanvasAutomation.h \
    CanvasBoard.h \
    CanvasInspector.h \
    CanvasItems.h \
    CanvasLibrary.h \
    CanvasModel.h \
    CanvasPage.h \
    CanvasView.h \
    CanvasWorkflow.h \
    CanvasWorkspace.h \
    ClaudeCli.h \
    CSharpMetrics.h \
    ClassDiagram.h \
    ClaudeCodeLogin.h \
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
    FolderLauncher.h \
    FontPickerDialog.h \
    GitHistory.h \
    GitProcess.h \
    HoverInfoPopup.h \
    IntegrationsDialog.h \
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
    SecretStore.h \
    SidePanelDock.h \
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
    Theme.h \
    TaskMaterials.h \
    TransitionPolicy.h \
    VerticalTabButton.h \
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

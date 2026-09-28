# core-private: QZipReader/QZipWriter (lampiran .xlsx/.docx dan fixture-nya)
QT += testlib gui core-private
CONFIG += no_private_qt_headers_warning

CONFIG += console c++17 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_swarm

INCLUDEPATH += ..

SOURCES += \
    tst_swarm.cpp \
    ../ClaudeCli.cpp \
    ../CSharpMetrics.cpp \
    ../ClassDiagram.cpp \
    ../ClaudeCodeRuntime.cpp \
    ../CodeMetrics.cpp \
    ../DocumentText.cpp \
    ../EdgeMermaidRenderer.cpp \
    ../FileManager.cpp \
    ../GitProcess.cpp \
    ../MermaidRenderer.cpp \
    ../PromptComposer.cpp \
    ../RunLogFormatter.cpp \
    ../SourceTokens.cpp \
    ../StageCatalog.cpp \
    ../StageProfile.cpp \
    ../StageSwarm.cpp \
    ../StreamJsonParser.cpp \
    ../SwarmCoordinator.cpp \
    ../TaskAttachments.cpp \
    ../TaskGit.cpp \
    ../TaskManager.cpp \
    ../TransitionPolicy.cpp \
    ../WorkspaceDiff.cpp \
    ../WorkspaceGuard.cpp

HEADERS += \
    FakeAgentRuntime.h \
    GitSandbox.h \
    ../AgentDefinition.h \
    ../AgentRuntime.h \
    ../AgentTypes.h \
    ../ClaudeCli.h \
    ../CSharpMetrics.h \
    ../ClassDiagram.h \
    ../ClaudeCodeRuntime.h \
    ../CodeMetrics.h \
    ../DocumentText.h \
    ../EdgeMermaidRenderer.h \
    ../FileManager.h \
    ../GitProcess.h \
    ../MermaidRenderer.h \
    ../PromptComposer.h \
    ../RunLogFormatter.h \
    ../SourceTokens.h \
    ../StageCatalog.h \
    ../StageProfile.h \
    ../StageSwarm.h \
    ../StreamJsonParser.h \
    ../SwarmCoordinator.h \
    ../TaskAttachments.h \
    ../TaskGit.h \
    ../TaskItem.h \
    ../TaskManager.h \
    ../TaskMaterials.h \
    ../TransitionPolicy.h \
    ../WorkspaceDiff.h \
    ../WorkspaceGuard.h

# Prompt peran (:/prompts), mermaid.min.js (:/vendor), dan fixture stream-json (:/fixtures)
RESOURCES += \
    ../Resources/resources.qrc \
    fixtures.qrc

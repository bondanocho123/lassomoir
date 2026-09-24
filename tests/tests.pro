QT += testlib gui

CONFIG += console c++17 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_swarm

INCLUDEPATH += ..

SOURCES += \
    tst_swarm.cpp \
    ../ClaudeCli.cpp \
    ../CSharpMetrics.cpp \
    ../ClaudeCodeRuntime.cpp \
    ../CodeMetrics.cpp \
    ../EdgeMermaidRenderer.cpp \
    ../FileManager.cpp \
    ../MermaidRenderer.cpp \
    ../PromptComposer.cpp \
    ../RunLogFormatter.cpp \
    ../SourceTokens.cpp \
    ../StageCatalog.cpp \
    ../StageProfile.cpp \
    ../StageSwarm.cpp \
    ../StreamJsonParser.cpp \
    ../SwarmCoordinator.cpp \
    ../TaskManager.cpp \
    ../TransitionPolicy.cpp \
    ../WorkspaceDiff.cpp \
    ../WorkspaceGuard.cpp

HEADERS += \
    FakeAgentRuntime.h \
    ../AgentDefinition.h \
    ../AgentRuntime.h \
    ../AgentTypes.h \
    ../ClaudeCli.h \
    ../CSharpMetrics.h \
    ../ClaudeCodeRuntime.h \
    ../CodeMetrics.h \
    ../EdgeMermaidRenderer.h \
    ../FileManager.h \
    ../MermaidRenderer.h \
    ../PromptComposer.h \
    ../RunLogFormatter.h \
    ../SourceTokens.h \
    ../StageCatalog.h \
    ../StageProfile.h \
    ../StageSwarm.h \
    ../StreamJsonParser.h \
    ../SwarmCoordinator.h \
    ../TaskItem.h \
    ../TaskManager.h \
    ../TransitionPolicy.h \
    ../WorkspaceDiff.h \
    ../WorkspaceGuard.h

# Prompt peran (:/prompts), mermaid.min.js (:/vendor), dan fixture stream-json (:/fixtures)
RESOURCES += \
    ../Resources/resources.qrc \
    fixtures.qrc

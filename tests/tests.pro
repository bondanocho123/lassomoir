QT += testlib gui

CONFIG += console c++17 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_swarm

INCLUDEPATH += ..

SOURCES += \
    tst_swarm.cpp \
    ../ClaudeCli.cpp \
    ../ClaudeCodeRuntime.cpp \
    ../EdgeMermaidRenderer.cpp \
    ../FileManager.cpp \
    ../MermaidRenderer.cpp \
    ../PromptComposer.cpp \
    ../RunLogFormatter.cpp \
    ../StageCatalog.cpp \
    ../StageProfile.cpp \
    ../StageSwarm.cpp \
    ../StreamJsonParser.cpp \
    ../SwarmCoordinator.cpp \
    ../TaskManager.cpp \
    ../TransitionPolicy.cpp \
    ../WorkspaceGuard.cpp

HEADERS += \
    FakeAgentRuntime.h \
    ../AgentDefinition.h \
    ../AgentRuntime.h \
    ../AgentTypes.h \
    ../ClaudeCli.h \
    ../ClaudeCodeRuntime.h \
    ../EdgeMermaidRenderer.h \
    ../FileManager.h \
    ../MermaidRenderer.h \
    ../PromptComposer.h \
    ../RunLogFormatter.h \
    ../StageCatalog.h \
    ../StageProfile.h \
    ../StageSwarm.h \
    ../StreamJsonParser.h \
    ../SwarmCoordinator.h \
    ../TaskItem.h \
    ../TaskManager.h \
    ../TransitionPolicy.h \
    ../WorkspaceGuard.h

# Prompt peran (:/prompts), mermaid.min.js (:/vendor), dan fixture stream-json (:/fixtures)
RESOURCES += \
    ../Resources/resources.qrc \
    fixtures.qrc

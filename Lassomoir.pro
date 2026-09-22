QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    ConsolePanelWidget.cpp \
    FileManager.cpp \
    KanbanCardWidget.cpp \
    KanbanColumnWidget.cpp \
    StageProfile.cpp \
    SwimlaneWidget.cpp \
    TaskManager.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    ConsolePanelWidget.h \
    FileManager.h \
    KanbanCardWidget.h \
    KanbanColumnWidget.h \
    StageProfile.h \
    SwimlaneWidget.h \
    TaskItem.h \
    TaskManager.h \
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

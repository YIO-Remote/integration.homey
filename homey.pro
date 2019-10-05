TEMPLATE        = lib
CONFIG         += plugin
QT             += websockets core quick
HEADERS         = homey.h \
                  ../remote-software/sources/integrations/integration.h \
                  ../remote-software/sources/integrations/integrationinterface.h
SOURCES         = homey.cpp
TARGET          = homey
DESTDIR         = ../remote-software/plugins

# install
unix {
    target.path = /usr/lib
    INSTALLS += target
}

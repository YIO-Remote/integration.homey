/******************************************************************************
 *
 * Copyright (C) 2019-2020 Marton Borzak <hello@martonborzak.com>
 * Copyright (C) 2019 Christian Riedl <ric@rts.co.at>
 * Copyright (C) 2019 Niels de Klerk <hello@martonborzak.com>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#include "homey.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtDebug>

#include "math.h"
#include "yio-interface/entities/blindinterface.h"
#include "yio-interface/entities/climateinterface.h"
#include "yio-interface/entities/lightinterface.h"
#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-interface/entities/switchinterface.h"

HomeyPlugin::HomeyPlugin() : Plugin("homey", USE_WORKER_THREAD) {}

Integration *HomeyPlugin::createIntegration(const QVariantMap &config, EntitiesInterface *entities,
                                            NotificationsInterface *notifications, YioAPIInterface *api,
                                            ConfigInterface *configObj) {
    qCInfo(m_logCategory) << "Creating Homey integration plugin" << PLUGIN_VERSION;

    return new Homey(config, entities, notifications, api, configObj, this);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// Homey THREAD CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Homey::Homey(const QVariantMap &config, EntitiesInterface *entities, NotificationsInterface *notifications,
             YioAPIInterface *api, ConfigInterface *configObj, Plugin *plugin)
    : Integration(config, entities, notifications, api, configObj, plugin) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == Integration::OBJ_DATA) {
            QVariantMap map = iter.value().toMap();
            m_ip            = map.value(Integration::KEY_DATA_IP).toString();
            m_token         = map.value(Integration::KEY_DATA_TOKEN).toString();
        }
    }

    m_api = api;

    // FIXME magic number
    m_webSocketId = 4;

    m_wsReconnectTimer = new QTimer(this);
    m_wsReconnectTimer->setSingleShot(true);
    m_wsReconnectTimer->setInterval(2000);
    m_wsReconnectTimer->stop();

    m_webSocket = new QWebSocket;
    m_webSocket->setParent(this);

    QObject::connect(m_webSocket, &QWebSocket::textFrameReceived, this, &Homey::onTextMessageReceived);
    QObject::connect(m_webSocket, static_cast<void (QWebSocket::*)(QAbstractSocket::SocketError)>(&QWebSocket::error),
                     this, &Homey::onError);
    QObject::connect(m_webSocket, &QWebSocket::stateChanged, this, &Homey::onStateChanged);

    QObject::connect(m_wsReconnectTimer, &QTimer::timeout, this, &Homey::onTimeout);
}

void Homey::onTextMessageReceived(const QString &message) {
    QJsonParseError parseerror;
    QJsonDocument   doc = QJsonDocument::fromJson(message.toUtf8(), &parseerror);
    if (parseerror.error != QJsonParseError::NoError) {
        qCCritical(m_logCategory) << "JSON error:" << parseerror.errorString();
        return;
    }
    QVariantMap map = doc.toVariant().toMap();

    QString m = map.value("error").toString();
    if (m.length() > 0) {
        qCCritical(m_logCategory) << "Message error:" << m;
    }

    QString type = map.value("type").toString();

    if (type == "connected") {
        setState(CONNECTED);
    }

    if (type == "command" && map.value("command").toString() == "getEntities") {
        // get loaded homey entities
        QList<EntityInterface *> es = m_entities->getByIntegration(integrationId());

        // create return map object
        QVariantMap returnData;

        // set type
        returnData.insert("type", "getEntities");

        // create list to store entity ids
        QStringList list;

        // interate throug the list and get the entity ids

        for (EntityInterface *value : es) {
            list.append(value->entity_id());
            qCDebug(m_logCategory) << value->entity_id();
        }
        qCDebug(m_logCategory) << "LIST" << list;
        // insert list to data key in response
        returnData.insert("devices", list);

        // convert map to json
        QJsonDocument doc     = QJsonDocument::fromVariant(returnData);
        QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);

        // send message
        m_webSocket->sendTextMessage(message);
    }

    // get all the entities from the homey app
    if (type == "sendEntities") {
        QVariantList availableEntities = map.value("available_entities").toList();

        bool success = true;

        for (int i = 0; i < availableEntities.length(); i++) {
            // add entity to allAvailableEntities list
            QVariantMap entity = availableEntities[i].toMap();
            entity.insert("integration", integrationId());
            if (!addAvailableEntity(entity.value("entity_id").toString(), entity.value("type").toString(),
                                    entity.value("integration").toString(), entity.value("friendly_name").toString(),
                                    entity.value("supported_features").toStringList())) {
                qCWarning(m_logCategory) << "Failed to add entity to the available entities list:"
                                         << entity.value("entity_id").toString();
                success = false;
            }

            // create an entity
            if (!m_api->addEntity(entity)) {
                qCWarning(m_logCategory) << "Failed to create entity:" << entity.value("entity_id").toString();
                success = false;
            }
        }

        if (!success) {
            m_notifications->add(true, tr("Failed to add entities from: %1").arg(friendlyName()));
        }
    }

    // handle fetch states from homey app
    if (type == "sendStates") {
        QVariantMap data = map.value("data").toMap();
        updateEntity(data.value("entity_id").toString(), data);
    }

    if (type == "event") {
        QVariantMap data = map.value("data").toMap();
        updateEntity(data.value("entity_id").toString(), data);
    }
}

void Homey::onStateChanged(QAbstractSocket::SocketState state) {
    if (state == QAbstractSocket::UnconnectedState && !m_userDisconnect) {
        qCDebug(m_logCategory) << "State changed to 'Unconnected': starting reconnect";
        if (m_webSocket->isValid()) {
            m_webSocket->close();
        }
        setState(DISCONNECTED);
        m_wsReconnectTimer->start();
    }
}

void Homey::onError(QAbstractSocket::SocketError error) {
    qCWarning(m_logCategory) << error << m_webSocket->errorString();
    if (m_webSocket->isValid()) {
        m_webSocket->close();
    }
    setState(DISCONNECTED);
    m_wsReconnectTimer->start();
}

void Homey::onTimeout() {
    if (m_tries == 3) {
        m_wsReconnectTimer->stop();

        qCCritical(m_logCategory) << "Cannot connect to Homey: retried 3 times connecting to" << m_ip;

        QObject *param = this;
        m_notifications->add(
            true, tr("Cannot connect to Homey."), tr("Reconnect"),
            [](QObject *param) {
                Integration *i = qobject_cast<Integration *>(param);
                i->connect();
            },
            param);

        disconnect();
        m_tries = 0;
    } else {
        // FIXME magic number
        m_webSocketId = 4;
        if (m_state != CONNECTING) {
            setState(CONNECTING);
        }

        QString url = QString("ws://").append(m_ip);
        qCDebug(m_logCategory) << "Reconnection attempt" << m_tries + 1 << "to Homey server:" << url;
        m_webSocket->open(QUrl(url));

        m_tries++;
    }
}

void Homey::webSocketSendCommand(const QVariantMap &data) {
    QJsonDocument doc     = QJsonDocument::fromVariant(data);
    QString       message = doc.toJson(QJsonDocument::JsonFormat::Compact);
    m_webSocket->sendTextMessage(message);
}

int Homey::convertBrightnessToPercentage(float value) { return static_cast<int>(round(value * 100)); }

void Homey::updateEntity(const QString &entity_id, const QVariantMap &attr) {
    EntityInterface *entity = m_entities->getEntityInterface(entity_id);
    if (entity) {
        if (entity->type() == "light") {
            updateLight(entity, attr);
        }
        if (entity->type() == "blind") {
            updateBlind(entity, attr);
        }
        if (entity->type() == "media_player") {
            updateMediaPlayer(entity, attr);
        }
        if (entity->type() == "climate") {
            updateClimate(entity, attr);
        }
        if (entity->type() == "switch") {
            updateSwitch(entity, attr);
        }
    }
}

void Homey::updateLight(EntityInterface *entity, const QVariantMap &attr) {
    // onoff to state.
    if (attr.contains("onoff")) {
        entity->setState(attr.value("onoff").toBool() ? LightDef::ON : LightDef::OFF);
    }

    // brightness
    if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
        if (attr.contains("dim")) {
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, convertBrightnessToPercentage(attr.value("dim").toFloat()));
        }
    }

    // color
    if (entity->isSupported(LightDef::F_COLOR)) {
        QVariant     color = attr.value("attributes").toMap().value("rgb_color");
        QVariantList cl(color.toList());
        char         buffer[10];
        snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", cl.value(0).toInt(), cl.value(1).toInt(),
                 cl.value(2).toInt());
        entity->updateAttrByIndex(LightDef::COLOR, buffer);
    }
}

void Homey::updateBlind(EntityInterface *entity, const QVariantMap &attr) {
    Q_UNUSED(entity);
    Q_UNUSED(attr);
    //    QVariantMap attributes;

    //    // state
    //    if (attr.value("state").toString() == "open") {
    //        attributes.insert("state", true);
    //    } else {
    //        attributes.insert("state", false);
    //    }

    //    // position
    //    if (entity->supported_features().indexOf("POSITION") > -1) {
    //        attributes.insert("position", attr.value("attributes").toMap().value("current_position").toInt());
    //    }

    //    m_entities->update(entity->entity_id(), attributes);
}

void Homey::updateMediaPlayer(EntityInterface *entity, const QVariantMap &attr) {
    /*  capabilities:
       [ 'speaker_album',
         'speaker_artist',
         'speaker_duration',
         'speaker_next',
         'speaker_playing',
         'speaker_position',
         'speaker_prev',
         'speaker_repeat',
         'volume_set',
         'volume_mute',
         'speaker_shuffle',
         'speaker_track',
         'sonos_group',
         'sonos_audio_clip' ]
    */
    // QVariantMap attributes;

    // state
    if (attr.contains("speaker_playing")) {
        if (attr.value("speaker_playing").toBool()) {
            entity->setState(MediaPlayerDef::PLAYING);
        } else {
            entity->setState(MediaPlayerDef::IDLE);
        }
    }

    if (attr.contains("onoff")) {
        if (attr.value("onoff").toBool()) {
            entity->setState(MediaPlayerDef::ON);
        } else {
            entity->setState(MediaPlayerDef::OFF);
        }
    }

    // FIXME
    // source
    // if (entity->supported_features().indexOf("SOURCE") > -1 && attr.value("attributes").toMap().contains("source")) {
    //    attributes.insert("source", attr.value("attributes").toMap().value("source").toString());
    //}

    // volume  //volume_set
    if (attr.contains("volume_set")) {
        entity->updateAttrByIndex(MediaPlayerDef::VOLUME,
                                  static_cast<int>(round(attr.value("volume_set").toDouble() * 100)));
    }

    // media type
    if (entity->isSupported(MediaPlayerDef::F_MEDIA_TYPE) &&
        attr.value("attributes").toMap().contains("media_content_type")) {
        entity->updateAttrByIndex(MediaPlayerDef::MEDIATYPE,
                                  attr.value("attributes").toMap().value("media_content_type").toString());
    }

    // media image
    if (attr.contains("album_art")) {
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, attr.value("album_art"));
    }

    // media title
    if (attr.contains("speaker_track")) {
        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, attr.value("speaker_track").toString());
    }

    // media artist
    if (attr.contains("speaker_artist")) {
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, attr.value("speaker_artist").toString());
    }
}

void Homey::updateClimate(EntityInterface *entity, const QVariantMap &attr) {
    // FIXME
    Q_UNUSED(entity);
    Q_UNUSED(attr);
}

void Homey::updateSwitch(EntityInterface *entity, const QVariantMap &attr) {
    // onoff to state.
    if (attr.contains("onoff")) {
        entity->setState(attr.value("onoff").toBool() ? SwitchDef::ON : SwitchDef::OFF);
    }
}

void Homey::connect() {
    m_userDisconnect = false;

    setState(CONNECTING);

    // reset the reconnnect trial variable
    m_tries = 0;

    // turn on the websocket connection
    QString url = QString("ws://").append(m_ip);
    qCDebug(m_logCategory) << "Connecting to Homey server:" << url;
    m_webSocket->open(QUrl(url));
}

void Homey::disconnect() {
    m_userDisconnect = true;
    qCDebug(m_logCategory) << "Disconnecting from Homey";

    // turn of the reconnect try
    m_wsReconnectTimer->stop();

    // turn off the socket
    m_webSocket->close();

    setState(DISCONNECTED);
}

void Homey::sendCommand(const QString &type, const QString &entityId, int command, const QVariant &param) {
    // example
    // {"command":"onoff","deviceId":"78f3ab16-c622-4bd7-aebf-3ca981e41375","type":"command","value":true}

    // TODO(zehnm) enhance webSocketSendCommand with command / value arguments to reduce QVariantMap overhead
    QVariantMap map;
    map.insert("type", "command");
    map.insert("deviceId", QVariant(entityId));

    if (type == "light") {
        if (command == LightDef::C_TOGGLE) {
            map.insert("command", QVariant("toggle"));
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_ON) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_OFF) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", false);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_BRIGHTNESS) {
            map.insert("command", "dim");
            float value = param.toFloat() / 100;
            map.insert("value", value);
            webSocketSendCommand(map);
        } else if (command == LightDef::C_COLOR) {
            QColor color = param.value<QColor>();
            // QVariantMap data;
            QVariantList list;
            list.append(color.red());
            list.append(color.green());
            list.append(color.blue());
            map.insert("command", "color");
            map.insert("value", list);
            webSocketSendCommand(map);
            // webSocketSendCommand(type, "turn_on", entity_id, &data);
        }
    } else if (type == "blind") {
        if (command == BlindDef::C_OPEN) {
            map.insert("command", "windowcoverings_closed");
            map.insert("value", "false");
            webSocketSendCommand(map);
        } else if (command == BlindDef::C_CLOSE) {
            map.insert("command", "windowcoverings_closed");
            map.insert("value", "true");
            webSocketSendCommand(map);
        } else if (command == BlindDef::C_STOP) {
            map.insert("command", "windowcoverings_tilt_set");
            map.insert("value", 0);
            webSocketSendCommand(map);
        } else if (command == BlindDef::C_POSITION) {
            map.insert("command", "windowcoverings_set");
            map.insert("value", param);
            webSocketSendCommand(map);
        }
    } else if (type == "media_player") {
        if (command == MediaPlayerDef::C_VOLUME_SET) {
            map.insert("command", "volume_set");
            map.insert("value", param.toDouble() / 100);
            QVariantMap attributes;
            attributes.insert("volume", param);
            m_entities->update(entityId, attributes);  // buggy homey fix
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_PLAY) {
            map.insert("command", "speaker_playing");
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_STOP) {
            map.insert("command", "speaker_playing");
            map.insert("value", false);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_PAUSE) {
            map.insert("command", "speaker_playing");
            map.insert("value", false);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_PREVIOUS) {
            map.insert("command", "speaker_prev");
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_NEXT) {
            map.insert("command", "speaker_next");
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_TURNON) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", true);
            webSocketSendCommand(map);
        } else if (command == MediaPlayerDef::C_TURNOFF) {
            map.insert("command", QVariant("onoff"));
            map.insert("value", false);
            webSocketSendCommand(map);
        }
    }
}

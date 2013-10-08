/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

import QtQuick 2.0
import HelperWidgets 2.0
import QtQuick.Layouts 1.0

Rectangle {
    id: itemPane
    width: 320
    height: 400
    color: "#404040"

    ScrollView {
        anchors.fill: parent

        Column {
            width: itemPane.width
            Section {
                caption: qsTr("Type")

                anchors.left: parent.left
                anchors.right: parent.right

                SectionLayout {
                    Label {
                        text: qsTr("Type")

                    }

                    Label {
                        text: backendValues.className.value
                        width: lineEdit.width
                    }

                    Label {
                        text: qsTr("id")

                    }

                    SecondColumnLayout {
                        LineEdit {
                            id: lineEdit
                            enabled: isBaseState
                            backendValue: backendValues.id
                            placeholderText: qsTr("id")
                            text: backendValues.id.value
                            Layout.fillWidth: true
                            Layout.maximumWidth: 320
                        }

                        ExpandingSpacer {
                        }

                        Item {
                            width: 16
                            height: 16
                        }
                    }
                }
            }

            GeometrySection {
            }

            Section {
                anchors.left: parent.left
                anchors.right: parent.right

                caption: qsTr("Visibilty")

                SectionLayout {
                    rows: 2
                    Label {
                        text: qsTr("Visibilty")
                    }

                    SecondColumnLayout {

                        CheckBox {
                            text: qsTr("Is Visible")
                            backendValue: backendValues.visible
                        }

                        Item {
                            width: 10
                            height: 10

                        }

                        CheckBox {
                            text: qsTr("Clip")
                            backendValue: backendValues.clip
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }

                    Label {
                        text: qsTr("Opacity")
                    }

                    SecondColumnLayout {
                        SpinBox {
                            backendValue: backendValues.opacity
                            decimals: 2

                            minimumValue: 0
                            maximumValue: 1
                            hasSlider: true
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            TabView {
                anchors.left: parent.left
                anchors.right: parent.right
                frameVisible: false

                Tab {
                    title: backendValues.className.value

                    component: Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        //                    Loader {
                        //                        id: specificsTwo;
                        //                        baseUrl: globalBaseUrl;
                        //                        qmlData: specificQmlData;
                        //                    }

                        Loader {
                            anchors.left: parent.left
                            anchors.right: parent.right

                            id: specificsOne;
                            source: specificsUrl;
                        }
                    }
                }

                Tab {
                    title: qsTr("Layout")
                    component: Column {
                        anchors.left: parent.left
                        anchors.right: parent.right

                        LayoutSection {
                        }
                    }
                }

                Tab {
                    anchors.fill: parent
                    title: qsTr("Advanced")
                    component: Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        AdvancedSection {
                        }
                    }
                }
            }
        }
    }
}

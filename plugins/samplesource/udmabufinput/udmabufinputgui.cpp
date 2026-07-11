///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2015-2022 Edouard Griffiths, F4EXB <f4exb06@gmail.com>          //
// Copyright (C) 2018 beta-tester <alpha-beta-release@gmx.net>                   //
// Copyright (C) 2018 Jason Gerecke <killertofu@gmail.com>                       //
// Copyright (C) 2021-2023 Jon Beniston, M7RCE <jon@beniston.com>                //
// Copyright (C) 2021 Andreas Baulig <free.geronimo@hotmail.de>                  //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <QDebug>

#include <QTime>
#include <QDateTime>
#include <QString>
#include <QFileDialog>
#include <QMessageBox>

#include "ui_udmabufinputgui.h"
#include "gui/glspectrum.h"
#include "gui/basicdevicesettingsdialog.h"
#include "gui/dialogpositioner.h"
#include "mainspectrum/mainspectrumgui.h"
#include "dsp/dspcommands.h"

#include "udmabufinputgui.h"
#include "device/deviceapi.h"
#include "device/deviceuiset.h"

UdmaBufInputGUI::UdmaBufInputGUI(DeviceUISet *deviceUISet, QWidget* parent) :
	DeviceGUI(parent),
	ui(new Ui::UdmaBufInputGUI),
	m_settings(),
	m_doApplySettings(true),
	m_sampleSource(0),
	m_acquisition(false),
	m_sampleRate(0),
	m_centerFrequency(0),
	m_recordLengthMuSec(0),
	m_startingTimeStamp(0),
	m_samplesCount(0),
	m_tickCount(0),
	m_enableNavTime(false),
	m_lastEngineState(DeviceAPI::StNotStarted)
{
    m_deviceUISet = deviceUISet;
    setAttribute(Qt::WA_DeleteOnClose, true);
    ui->setupUi(getContents());
    sizeToContents();
    getContents()->setStyleSheet("#UdmaBufInputGUI { background-color: rgb(64, 64, 64); }");
    m_helpURL = "plugins/samplesource/udmabufinput/readme.md";
	ui->crcLabel->setStyleSheet("QLabel { background:rgb(79,79,79); }");

	connect(&(m_deviceUISet->m_deviceAPI->getMasterTimer()), SIGNAL(timeout()), this, SLOT(tick()));
	connect(&m_statusTimer, SIGNAL(timeout()), this, SLOT(updateStatus()));
	m_statusTimer.start(500);

    connect(this, SIGNAL(customContextMenuRequested(const QPoint &)), this, SLOT(openDeviceSettingsDialog(const QPoint &)));

	setAccelerationCombo();
	displaySettings();

	ui->navTimeSlider->setEnabled(false);
	ui->acceleration->setEnabled(false);
	ui->playLoop->setEnabled(false);

    m_sampleSource = m_deviceUISet->m_deviceAPI->getSampleSource();

    connect(&m_inputMessageQueue, SIGNAL(messageEnqueued()), this, SLOT(handleInputMessages()), Qt::QueuedConnection);
    m_sampleSource->setMessageQueueToGUI(&m_inputMessageQueue);

    makeUIConnections();
    m_resizer.enableChildMouseTracking();
}

UdmaBufInputGUI::~UdmaBufInputGUI()
{
    qDebug("UdmaBufInputGUI::~UdmaBufInputGUI");
    m_statusTimer.stop();
	delete ui;
    qDebug("UdmaBufInputGUI::~UdmaBufInputGUI: end");
}

void UdmaBufInputGUI::destroy()
{
	delete this;
}

void UdmaBufInputGUI::resetToDefaults()
{
	m_settings.resetToDefaults();
	displaySettings();
	sendSettings();
}

QByteArray UdmaBufInputGUI::serialize() const
{
	return m_settings.serialize();
}

bool UdmaBufInputGUI::deserialize(const QByteArray& data)
{
	if(m_settings.deserialize(data)) {
		displaySettings();
		sendSettings();
		return true;
	} else {
		resetToDefaults();
		return false;
	}
}

void UdmaBufInputGUI::handleInputMessages()
{
    Message* message;

    while ((message = m_inputMessageQueue.pop()) != 0)
    {
        if (DSPSignalNotification::match(*message))
        {
            DSPSignalNotification* notif = (DSPSignalNotification*) message;
            m_deviceSampleRate = notif->getSampleRate();
            m_deviceCenterFrequency = notif->getCenterFrequency();
            qDebug("UdmaBufInputGUI::handleInputMessages: DSPSignalNotification: SampleRate:%d, CenterFrequency:%llu", notif->getSampleRate(), notif->getCenterFrequency());
            updateSampleRateAndFrequency();

            delete message;
        }
        else
        {
            if (handleMessage(*message))
            {
                delete message;
            }
        }
    }
}

bool UdmaBufInputGUI::handleMessage(const Message& message)
{
    if (UdmaBufInput::MsgConfigureUdmaBufInput::match(message))
    {
        const UdmaBufInput::MsgConfigureUdmaBufInput& cfg = (UdmaBufInput::MsgConfigureUdmaBufInput&) message;

        if (cfg.getForce()) {
            m_settings = cfg.getSettings();
        } else {
            m_settings.applySettings(cfg.getSettingsKeys(), cfg.getSettings());
        }

        displaySettings();
        return true;
    }
    else if (UdmaBufInput::MsgReportFileSourceAcquisition::match(message))
	{
		m_acquisition = ((UdmaBufInput::MsgReportFileSourceAcquisition&)message).getAcquisition();
		updateWithAcquisition();
		return true;
	}
	else if (UdmaBufInput::MsgReportUdmaBufInputStreamData::match(message))
	{
		m_sampleRate = ((UdmaBufInput::MsgReportUdmaBufInputStreamData&)message).getSampleRate();
		m_sampleSize = ((UdmaBufInput::MsgReportUdmaBufInputStreamData&)message).getSampleSize();
		m_centerFrequency = ((UdmaBufInput::MsgReportUdmaBufInputStreamData&)message).getCenterFrequency();
		m_startingTimeStamp = ((UdmaBufInput::MsgReportUdmaBufInputStreamData&)message).getStartingTimeStamp();
		m_recordLengthMuSec = ((UdmaBufInput::MsgReportUdmaBufInputStreamData&)message).getRecordLengthMuSec();
		updateWithStreamData();
		return true;
	}
	else if (UdmaBufInput::MsgReportUdmaBufInputStreamTiming::match(message))
	{
		m_samplesCount = ((UdmaBufInput::MsgReportUdmaBufInputStreamTiming&)message).getSamplesCount();
		updateWithStreamTime();
		return true;
	}
	else if (UdmaBufInput::MsgStartStop::match(message))
    {
	    UdmaBufInput::MsgStartStop& notif = (UdmaBufInput::MsgStartStop&) message;
        blockApplySettings(true);
        ui->startStop->setChecked(notif.getStartStop());
        blockApplySettings(false);

        return true;
    }
	else if (UdmaBufInput::MsgPlayPause::match(message))
	{
	    UdmaBufInput::MsgPlayPause& notif = (UdmaBufInput::MsgPlayPause&) message;
	    bool checked = notif.getPlayPause();
	    ui->play->setChecked(checked);
	    ui->navTimeSlider->setEnabled(false);
	    ui->acceleration->setEnabled(false);
	    m_enableNavTime = false;

	    return true;
	}
	else if (UdmaBufInput::MsgReportHeaderCRC::match(message))
	{
		UdmaBufInput::MsgReportHeaderCRC& notif = (UdmaBufInput::MsgReportHeaderCRC&) message;
		if (notif.isOK()) {
			ui->crcLabel->setStyleSheet("QLabel { background-color : green; }");
		} else {
			ui->crcLabel->setStyleSheet("QLabel { background-color : red; }");
		}

		return true;
	}
	else
	{
		return false;
	}
}

void UdmaBufInputGUI::updateSampleRateAndFrequency()
{
    m_deviceUISet->getSpectrum()->setSampleRate(m_deviceSampleRate);
    m_deviceUISet->getSpectrum()->setCenterFrequency(m_deviceCenterFrequency);
    ui->deviceRateText->setText(tr("%1k").arg((float)m_deviceSampleRate / 1000));
}

void UdmaBufInputGUI::displaySettings()
{
    blockApplySettings(true);
    setTitle(m_settings.m_title);
    m_deviceUISet->m_mainSpectrumGUI->setTitle(m_settings.m_title);
    ui->playLoop->setChecked(m_settings.m_loop);
    ui->acceleration->setCurrentIndex(UdmaBufInputSettings::getAccelerationIndex(m_settings.m_accelerationFactor));

    if (!m_settings.m_fileName.isEmpty() && (m_settings.m_fileName != ui->fileNameText->text()))
    {
        ui->crcLabel->setStyleSheet("QLabel { background:rgb(79,79,79); }");
        configureFileName();
    }

    ui->fileNameText->setText(m_settings.m_fileName);
    blockApplySettings(false);
}

void UdmaBufInputGUI::sendSettings()
{
}

void UdmaBufInputGUI::on_playLoop_toggled(bool checked)
{
    if (m_doApplySettings)
    {
        m_settings.m_loop = checked;
        UdmaBufInput::MsgConfigureUdmaBufInput *message = UdmaBufInput::MsgConfigureUdmaBufInput::create(m_settings, QList<QString>{"loop"}, false);
        m_sampleSource->getInputMessageQueue()->push(message);
    }
}

void UdmaBufInputGUI::on_startStop_toggled(bool checked)
{
    if (m_doApplySettings)
    {
        UdmaBufInput::MsgStartStop *message = UdmaBufInput::MsgStartStop::create(checked);
        m_sampleSource->getInputMessageQueue()->push(message);
    }
}

void UdmaBufInputGUI::updateStatus()
{
    int state = m_deviceUISet->m_deviceAPI->state();

    if(m_lastEngineState != state)
    {
        switch(state)
        {
            case DeviceAPI::StNotStarted:
                ui->startStop->setStyleSheet("QToolButton { background:rgb(79,79,79); }");
                break;
            case DeviceAPI::StIdle:
                ui->startStop->setStyleSheet("QToolButton { background-color : blue; }");
                break;
            case DeviceAPI::StRunning:
                ui->startStop->setStyleSheet("QToolButton { background-color : green; }");
                break;
            case DeviceAPI::StError:
                ui->startStop->setStyleSheet("QToolButton { background-color : red; }");
                QMessageBox::information(this, tr("Message"), m_deviceUISet->m_deviceAPI->errorMessage());
                break;
            default:
                break;
        }

        m_lastEngineState = state;
    }
}

void UdmaBufInputGUI::on_play_toggled(bool checked)
{
	UdmaBufInput::MsgConfigureUdmaBufInputWork* message = UdmaBufInput::MsgConfigureUdmaBufInputWork::create(checked);
	m_sampleSource->getInputMessageQueue()->push(message);
	ui->navTimeSlider->setEnabled(false);
	ui->acceleration->setEnabled(false);
	m_enableNavTime = false;
}

void UdmaBufInputGUI::on_navTimeSlider_valueChanged(int value)
{
	if (m_enableNavTime && ((value >= 0) && (value <= 1000)))
	{
		UdmaBufInput::MsgConfigureFileSourceSeek* message = UdmaBufInput::MsgConfigureFileSourceSeek::create(value);
		m_sampleSource->getInputMessageQueue()->push(message);
	}
}

void UdmaBufInputGUI::on_showFileDialog_clicked(bool checked)
{
    (void) checked;
	QString fileName = QFileDialog::getOpenFileName(this,
	    tr("Open mmap I/Q ring"), QFileInfo(m_settings.m_fileName).dir().path(), tr("mmap I/Q rings (*.udma *.bin);;All files (*)"), 0);


	if (fileName != "")
	{
		m_settings.m_fileName = fileName;
		ui->fileNameText->setText(m_settings.m_fileName);
		ui->crcLabel->setStyleSheet("QLabel { background:rgb(79,79,79); }");
		configureFileName();
	}
}

void UdmaBufInputGUI::on_acceleration_currentIndexChanged(int index)
{
    if (m_doApplySettings)
    {
        m_settings.m_accelerationFactor = UdmaBufInputSettings::getAccelerationValue(index);
        UdmaBufInput::MsgConfigureUdmaBufInput *message = UdmaBufInput::MsgConfigureUdmaBufInput::create(m_settings, QList<QString>{"accelerationFactor"}, false);
        m_sampleSource->getInputMessageQueue()->push(message);
    }
}

void UdmaBufInputGUI::configureFileName()
{
	qDebug() << "UdmaBufInputGUI::configureFileName: " << m_settings.m_fileName.toStdString().c_str();
	UdmaBufInput::MsgConfigureFileSourceName* message = UdmaBufInput::MsgConfigureFileSourceName::create(m_settings.m_fileName);
	m_sampleSource->getInputMessageQueue()->push(message);
}

void UdmaBufInputGUI::updateWithAcquisition()
{
	ui->play->setEnabled(m_acquisition);
	ui->play->setChecked(m_acquisition);
	ui->showFileDialog->setEnabled(!m_acquisition);
}

void UdmaBufInputGUI::updateWithStreamData()
{
	ui->centerFrequency->setText(tr("%L1").arg(m_centerFrequency));
	ui->sampleRateText->setText(tr("%1k").arg((float)m_sampleRate / 1000));
	ui->sampleSizeText->setText(tr("%1b").arg(m_sampleSize));
	ui->play->setEnabled(m_acquisition);
	QTime recordLength(0, 0, 0, 0);
	recordLength = recordLength.addMSecs(m_recordLengthMuSec/1000UL);
	QString s_time = recordLength.toString("HH:mm:ss.zzz");
	ui->recordLengthText->setText(s_time);
	updateWithStreamTime();
}

void UdmaBufInputGUI::updateWithStreamTime()
{
    qint64 t_sec = 0;
    qint64 t_msec = 0;

	if (m_sampleRate > 0)
	{
		t_sec = m_samplesCount / m_sampleRate;
        t_msec = (m_samplesCount - (t_sec * m_sampleRate)) * 1000LL / m_sampleRate;
	}

	QTime t(0, 0, 0, 0);
	t = t.addSecs(t_sec);
	t = t.addMSecs(t_msec);
	QString s_timems = t.toString("HH:mm:ss.zzz");
	ui->relTimeText->setText(s_timems);

    qint64 startingTimeStampMsec = m_startingTimeStamp;
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(startingTimeStampMsec);
    dt = dt.addSecs(t_sec);
    dt = dt.addMSecs(t_msec);
	QString s_date = dt.toString("yyyy-MM-dd HH:mm:ss.zzz");
	ui->absTimeText->setText(s_date);

	if (!m_enableNavTime && (m_recordLengthMuSec > 0))
	{
		float posRatio = (float) (t_sec*1000000L + t_msec*1000L) / (float) m_recordLengthMuSec;
		ui->navTimeSlider->setValue((int) (posRatio * 1000.0));
	}
}

void UdmaBufInputGUI::tick()
{
	if ((++m_tickCount & 0xf) == 0) {
		UdmaBufInput::MsgConfigureUdmaBufInputStreamTiming* message = UdmaBufInput::MsgConfigureUdmaBufInputStreamTiming::create();
		m_sampleSource->getInputMessageQueue()->push(message);
	}
}

void UdmaBufInputGUI::setAccelerationCombo()
{
    ui->acceleration->blockSignals(true);
    ui->acceleration->clear();
    ui->acceleration->addItem(QString("1"));

    for (unsigned int i = 0; i <= UdmaBufInputSettings::m_accelerationMaxScale; i++)
    {
        QString s;
        int m = pow(10.0, i);
        int x = 2*m;
        setNumberStr(x, s);
        ui->acceleration->addItem(s);
        x = 5*m;
        setNumberStr(x, s);
        ui->acceleration->addItem(s);
        x = 10*m;
        setNumberStr(x, s);
        ui->acceleration->addItem(s);
    }

    ui->acceleration->blockSignals(false);
}

void UdmaBufInputGUI::setNumberStr(int n, QString& s)
{
    if (n < 1000) {
        s = tr("%1").arg(n);
    } else if (n < 100000) {
        s = tr("%1k").arg(n/1000);
    } else if (n < 1000000) {
        s = tr("%1e5").arg(n/100000);
    } else if (n < 1000000000) {
        s = tr("%1M").arg(n/1000000);
    } else {
        s = tr("%1G").arg(n/1000000000);
    }
}

void UdmaBufInputGUI::openDeviceSettingsDialog(const QPoint& p)
{
    if (m_contextMenuType == ContextMenuDeviceSettings)
    {
        BasicDeviceSettingsDialog dialog(this);
        dialog.setUseReverseAPI(m_settings.m_useReverseAPI);
        dialog.setReverseAPIAddress(m_settings.m_reverseAPIAddress);
        dialog.setReverseAPIPort(m_settings.m_reverseAPIPort);
        dialog.setReverseAPIDeviceIndex(m_settings.m_reverseAPIDeviceIndex);
        dialog.setTitle(m_settings.m_title);
        dialog.setDefaultTitle(getDefaultTitle());

        dialog.move(p);
        new DialogPositioner(&dialog, false);
        dialog.exec();

        if (dialog.result() == QDialog::Accepted)
        {
            m_settings.m_title = dialog.getTitle();
            setTitle(m_settings.m_title);
            m_deviceUISet->m_mainSpectrumGUI->setTitle(m_settings.m_title);
            m_settings.m_useReverseAPI = dialog.useReverseAPI();
            m_settings.m_reverseAPIAddress = dialog.getReverseAPIAddress();
            m_settings.m_reverseAPIPort = dialog.getReverseAPIPort();
            m_settings.m_reverseAPIDeviceIndex = dialog.getReverseAPIDeviceIndex();
            m_settingsKeys.append("title");
            m_settingsKeys.append("useReverseAPI");
            m_settingsKeys.append("reverseAPIAddress");
            m_settingsKeys.append("reverseAPIPort");
            m_settingsKeys.append("reverseAPIDeviceIndex");

            sendSettings();
        }
    }

    resetContextMenuType();
}

void UdmaBufInputGUI::makeUIConnections()
{
    QObject::connect(ui->startStop, &ButtonSwitch::toggled, this, &UdmaBufInputGUI::on_startStop_toggled);
    QObject::connect(ui->playLoop, &ButtonSwitch::toggled, this, &UdmaBufInputGUI::on_playLoop_toggled);
    QObject::connect(ui->play, &ButtonSwitch::toggled, this, &UdmaBufInputGUI::on_play_toggled);
    QObject::connect(ui->navTimeSlider, &QSlider::valueChanged, this, &UdmaBufInputGUI::on_navTimeSlider_valueChanged);
    QObject::connect(ui->showFileDialog, &QPushButton::clicked, this, &UdmaBufInputGUI::on_showFileDialog_clicked);
    QObject::connect(ui->acceleration, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &UdmaBufInputGUI::on_acceleration_currentIndexChanged);
}

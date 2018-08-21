/*
 *    Copyright (C) 2018
 *    Matthias P. Braendli (matthias.braendli@mpb.li)
 *
 *    Copyright (C) 2017
 *    Albrecht Lohofener (albrechtloh@gmx.de)
 *
 *    This file is based on SDR-J
 *    Copyright (C) 2010, 2011, 2012
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *
 *    This file is part of the welle.io.
 *    Many of the ideas as implemented in welle.io are derived from
 *    other work, made available through the GNU general Public License.
 *    All copyrights of the original authors are recognized.
 *
 *    welle.io is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    welle.io is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with welle.io; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <QDebug>
#include <QSettings>

#include "CRadioController.h"
#ifdef HAVE_SOAPYSDR
#include "CSoapySdr.h"
#endif /* HAVE_SOAPYSDR */
#include "CInputFactory.h"
#include "CRAWFile.h"
#include "CRTL_TCP_Client.h"
#include "CSplashScreen.h"

#define AUDIOBUFFERSIZE 32768

CRadioController::CRadioController(QVariantMap& commandLineOptions, DABParams& params, QObject *parent)
#ifdef Q_OS_ANDROID
    : CRadioControllerSource(parent)
#else
    : QObject(parent)
#endif
    , commandLineOptions(commandLineOptions)
    , dabparams(params)
    , audioBuffer(2 * AUDIOBUFFERSIZE)
    , audio(audioBuffer)
{
    // Init the technical data
    resetTechnicalData();

    // Read channels from settings
    stationList.loadStations();
    stationList.sort();
    emit stationsChanged(stationList.getList());

    // Init timers
    connect(&stationTimer, &QTimer::timeout, this, &CRadioController::stationTimerTimeout);
    connect(&channelTimer, &QTimer::timeout, this, &CRadioController::channelTimerTimeout);

    connect(this, &CRadioController::switchToNextChannel,
            this, &CRadioController::nextChannel);

    connect(this, &CRadioController::ensembleAdded,
            this, &CRadioController::addtoEnsemble);

    connect(this, &CRadioController::ensembleNameUpdated,
            this, &CRadioController::nameofEnsemble);

    qRegisterMetaType<dab_date_time_t>("dab_date_time_t");
    connect(this, &CRadioController::dateTimeUpdated,
            this, &CRadioController::displayDateTime);

    isAutoPlay = false;
}

void CRadioController::resetTechnicalData(void)
{
    currentChannel = tr("Unknown");
    emit channelChanged();

    currentEnsemble = "";
    emit ensembleChanged();

    currentFrequency = 0;
    emit frequencyChanged();

    currentStation = "";
    emit stationChanged();

    currentStationType = "";
    emit stationTypChanged();

    currentLanguageType = "";
    emit languageTypeChanged();

    currentTitle = tr("No Station");
    emit titleChanged();

    currentText = "";
    emit textChanged();

    mErrorMsg = "";
    mIsSync = false;
    mIsFICCRC = false;
    mIsSignal = false;
    mSNR = 0;
    mFrequencyCorrection = 0;
    mFrequencyCorrectionPpm = NAN;
    mBitRate = 0;
    mAudioSampleRate = 0;
    mIsStereo = true;
    mIsDAB = true;
    mFrameErrors = 0;
    mRSErrors = 0;
    mAACErrors = 0;
    mGainCount = 0;
    mStationCount = 0;
    currentManualGain = 0;
    currentManualGainValue = std::numeric_limits<float>::lowest();
    currentVolume = 1.0;
    isChannelScan = false;
    isAGC = true;
    isHwAGC = true;
    isHwAGCSupported = false;

    motImage.loadFromData(nullptr, 0);
    emit motChanged(motImage);
}

void CRadioController::closeDevice()
{
    qDebug() << "RadioController:" << "Close device";

    my_rx.reset();
    device.reset();
    audio.reset();

    // Reset the technical data
    resetTechnicalData();

    emit deviceClosed();
}

void CRadioController::openDevice(CVirtualInput *new_device)
{
    if (device) {
        closeDevice();
    }
    device.reset(new_device);
    initialise();
}

void CRadioController::onEventLoopStarted()
{
#ifdef Q_OS_ANDROID
    QString dabDevice = "rtl_tcp";
#else
    QString dabDevice = "auto";
#endif

#ifdef HAVE_SOAPYSDR
    QString sdrDriverArgs;
    QString sdrAntenna;
    QString sdrClockSource;
#endif /* HAVE_SOAPYSDR */
    QString ipAddress = "127.0.0.1";
    uint16_t ipPort = 1234;
    QString rawFile = "";
    QString rawFileFormat = "auto";

    if(commandLineOptions["dabDevice"] != "")
        dabDevice = commandLineOptions["dabDevice"].toString();

#ifdef HAVE_SOAPYSDR
    if(commandLineOptions["sdr-driver-args"] != "")
        sdrDriverArgs = commandLineOptions["sdr-driver-args"].toString();

    if(commandLineOptions["sdr-antenna"] != "")
        sdrAntenna = commandLineOptions["sdr-antenna"].toString();

    if(commandLineOptions["sdr-clock-source"] != "")
        sdrClockSource = commandLineOptions["sdr-clock-source"].toString();
#endif /* HAVE_SOAPYSDR */

    if(commandLineOptions["ipAddress"] != "")
        ipAddress = commandLineOptions["ipAddress"].toString();

    if(commandLineOptions["ipPort"] != "")
        ipPort = static_cast<unsigned short>(commandLineOptions["ipPort"].toUInt());

    if(commandLineOptions["rawFile"] != "")
        rawFile = commandLineOptions["rawFile"].toString();

    if(commandLineOptions["rawFileFormat"] != "")
        rawFileFormat = commandLineOptions["rawFileFormat"].toString();

    // Init device
    CSplashScreen::ShowMessage(tr("Init radio receiver"));
    device.reset(CInputFactory::GetDevice(*this, dabDevice.toStdString()));

    // Set rtl_tcp settings
    if (device->getID() == CDeviceID::RTL_TCP) {
        CRTL_TCP_Client* RTL_TCP_Client = static_cast<CRTL_TCP_Client*>(device.get());

        RTL_TCP_Client->setIP(ipAddress.toStdString());
        RTL_TCP_Client->setPort(ipPort);
    }

    // Set rawfile settings
    if (device->getID() == CDeviceID::RAWFILE) {
        CRAWFile* RAWFile = static_cast<CRAWFile*>(device.get());

        RAWFile->setFileName(rawFile.toStdString(), rawFileFormat.toStdString());
    }

#ifdef HAVE_SOAPYSDR
    if (device->getID() == CDeviceID::SOAPYSDR) {
        CSoapySdr *sdr = (CSoapySdr*)device.get();

        if (!sdrDriverArgs.isEmpty()) {
            sdr->setDriverArgs(sdrDriverArgs.toStdString());
        }

        if (!sdrAntenna.isEmpty()) {
            sdr->setAntenna(sdrAntenna.toStdString());
        }

        if (!sdrClockSource.isEmpty()) {
            sdr->setClockSource(sdrClockSource.toStdString());
        }
    }
#endif /* HAVE_SOAPYSDR */

    initialise();

    CSplashScreen::Close();
}

void CRadioController::initialise(void)
{
    mGainCount = device->getGainCount();
    emit gainCountChanged(mGainCount);

    device->setHwAgc(isHwAGC);

    if (!isAGC) { // Manual AGC
        device->setAgc(false);
        device->setGain(currentManualGain);
        qDebug() << "RadioController:" << "AGC off";
    }
    else {
        device->setAgc(true);
        qDebug() << "RadioController:" << "AGC on";
    }

    audio.setVolume(currentVolume);

    my_rx = std::make_unique<RadioReceiver>(*this, *device, rro);
    my_rx->setReceiverOptions(rro);

    emit deviceReady();

    isHwAGCSupported = device->isHwAgcSupported();
    emit isHwAGCSupportedChanged(isHwAGCSupported);

    deviceName = QString::fromStdString(device->getName());
    emit deviceNameChanged();

    if(isAutoPlay) {
        play(autoChannel, autoStation);
    }
}

void CRadioController::play(QString Channel, QString Station)
{
    if(Channel == "")
        return;

    qDebug() << "RadioController:" << "Play channel:"
             << Channel << "station:" << Station;

    if (isChannelScan == true) {
        stopScan();
    }

    deviceRestart();
    setChannel(Channel, false);
    setStation(Station);

    // Store as last station
    QSettings Settings;
    QStringList StationElement;
    StationElement. append (Station);
    StationElement. append (Channel);
    Settings.setValue("lastchannel", StationElement);
}

void CRadioController::stop()
{
    if (device) {
        device->stop();
    }

    audio.reset();
}

void CRadioController::clearStations()
{
    //	Clear old channels
    emit stationsCleared();
    stationList.reset();
    emit stationsChanged(stationList.getList());

    // Save the channels
    stationList.saveStations();

    // Clear last station
    QSettings Settings;
    Settings.remove("lastchannel");
}

void CRadioController::setVolume(qreal Volume)
{
    currentVolume = Volume;

    audio.setVolume(Volume);

    emit volumeChanged(currentVolume);
}

void CRadioController::setChannel(QString Channel, bool isScan, bool Force)
{
    if (currentChannel != Channel || Force == true) {
        if (device && device->getID() == CDeviceID::RAWFILE) {
            currentChannel = "File";
            currentEnsemble = "";
            currentFrequency = 0;
        }
        else { // A real device
            currentChannel = Channel;
            currentEnsemble = "";

            // Convert channel into a frequency
            currentFrequency = channels.getFrequency(Channel.toStdString());

            if(currentFrequency != 0 && device) {
                qDebug() << "RadioController: Tune to channel" <<  Channel << "->" << currentFrequency/1e6 << "MHz";
                device->setFrequency(currentFrequency);
            }
        }

        decoderRestart(isScan);

        stationListStr.clear();
        emit channelChanged();
        emit ensembleChanged();
        emit frequencyChanged();
    }
}

void CRadioController::setManualChannel(QString Channel)
{
    // Play channel's first station, if available
    foreach(StationElement* station, stationList.getList())
    {
        if (station->getChannelName() == Channel)
        {
            QString stationName = station->getStationName();
            qDebug() << "RadioController: Play channel" <<  Channel << "and first station" << stationName;
            play(Channel, stationName);
            return;
        }
    }

    // Otherwise tune to channel and play first found station
    qDebug() << "RadioController: Tune to channel" <<  Channel;

    deviceRestart();

    currentTitle = tr("Tuning") + " ... " + Channel;
    emit titleChanged();

    currentStation = "";
    emit stationChanged();

    currentStationType = "";
    emit stationTypChanged();

    currentLanguageType = "";
    emit languageTypeChanged();

    currentText = "";
    emit textChanged();

    // Clear MOT
    motImage.loadFromData(nullptr, 0);
    emit motChanged(motImage);

    // Switch channel
    setChannel(Channel, false, true);
}

void CRadioController::startScan(void)
{
    qDebug() << "RadioController:" << "Start channel scan";

    deviceRestart();

    if(device && device->getID() == CDeviceID::RAWFILE) {
        currentTitle = tr("RAW File");
        const auto FirstChannel = QString::fromStdString(Channels::firstChannel);
        setChannel(FirstChannel, false); // Just a dummy
        emit scanStopped();
    }
    else
    {
        // Start with lowest frequency
        QString Channel = QString::fromStdString(Channels::firstChannel);
        setChannel(Channel, true);

        isChannelScan = true;
        mStationCount = 0;
        currentTitle = tr("Scanning") + " ... " + Channel
                + " (" + QString::number((1 * 100 / NUMBEROFCHANNELS)) + "%)";
        emit titleChanged();

        currentText = tr("Found channels") + ": " + QString::number(mStationCount);
        emit textChanged();

        currentStation = "";
        emit stationChanged();

        currentStationType = "";
        emit stationTypChanged();

        currentLanguageType = "";
        emit languageTypeChanged();

        emit scanProgress(0);
    }

    clearStations();
}

void CRadioController::stopScan(void)
{
    qDebug() << "RadioController:" << "Stop channel scan";

    currentTitle = tr("No Station");
    emit titleChanged();

    currentText = "";
    emit textChanged();

    isChannelScan = false;
    emit scanStopped();
}

QList<StationElement *> CRadioController::stations() const
{
    return stationList.getList();
}

void CRadioController::setHwAGC(bool isHwAGC)
{
    this->isHwAGC = isHwAGC;

    if (device) {
        device->setHwAgc(isHwAGC);
        qDebug() << "RadioController:" << (isHwAGC ? "HwAGC on" : "HwAGC off");
    }
    emit hwAgcChanged(isHwAGC);
}

void CRadioController::setAGC(bool isAGC)
{
    this->isAGC = isAGC;

    if (device) {
        device->setAgc(isAGC);

        if (!isAGC) {
            device->setGain(currentManualGain);
            qDebug() << "RadioController:" << "AGC off";
        }
        else {
            qDebug() << "RadioController:" <<  "AGC on";
        }
    }
    emit agcChanged(isAGC);
}

void CRadioController::disableCoarseCorrector(bool disable)
{
    rro.disable_coarse_corrector = disable;
    if (my_rx) {
        my_rx->setReceiverOptions(rro);
    }
}

void CRadioController::enableTIIDecode(bool enable)
{
    rro.decodeTII = enable;
    if (my_rx) {
        my_rx->setReceiverOptions(rro);
    }
}

void CRadioController::enableOldFFTWindowPlacement(bool old)
{
    rro.ofdmProcessorThreshold = old ?
        OLD_OFDM_PROCESSOR_THRESHOLD : NEW_OFDM_PROCESSOR_THRESHOLD;

    if (my_rx) {
        my_rx->setReceiverOptions(rro);
    }
}

void CRadioController::setFreqSyncMethod(int fsm_ix)
{
    rro.freqsyncMethod = static_cast<FreqsyncMethod>(fsm_ix);

    if (my_rx) {
        my_rx->setReceiverOptions(rro);
    }
}

void CRadioController::setGain(int Gain)
{
    currentManualGain = Gain;

    if (device) {
        currentManualGainValue = device->setGain(Gain);
        int32_t mGainCount_tmp = device->getGainCount();

        if(mGainCount != mGainCount_tmp) {
            mGainCount = mGainCount_tmp;
            emit gainCountChanged(mGainCount);
        }
    }
    else
    {
        currentManualGainValue = std::numeric_limits<float>::lowest();
    }

    emit gainValueChanged(currentManualGainValue);
    emit gainChanged(currentManualGain);
}

void CRadioController::setErrorMessage(QString Text)
{
    mErrorMsg = Text;
    emit showErrorMessage(Text);
}

void CRadioController::setErrorMessage(const std::string& head, const std::string& text)
{
    if (text.empty()) {
        setErrorMessage(tr(head.c_str()));
    }
    else {
        setErrorMessage(tr(head.c_str()) + ": " + QString::fromStdString(text));
    }
}

void CRadioController::setInfoMessage(QString Text)
{
    emit showInfoMessage(Text);
}

/********************
 * Private methods  *
 ********************/

void CRadioController::deviceRestart()
{
    bool isPlay = false;

    if(device) {
        isPlay = device->restart();
    }

    if(!isPlay) {
        qDebug() << "RadioController:" << "Radio device is not ready or does not exist.";
        emit showErrorMessage(tr("Radio device is not ready or does not exist."));
        return;
    }
}

void CRadioController::decoderRestart(bool isScan)
{
    //	The ofdm processor is "conditioned" to send one signal
    //	per "scanning tour". This signal is either "false"
    //	if we are pretty certain that the channel does not contain
    //	a signal, or "true" if there is a fair chance that the
    //	channel contains useful data
    if (my_rx) {
        my_rx->restart(isScan);
    }
}

void CRadioController::setStation(QString Station, bool Force)
{
    if(currentStation != Station || Force == true)
    {
        currentStation = Station;
        emit stationChanged();

        qDebug() << "RadioController: Tune to station" <<  Station;

        currentTitle = tr("Tuning") + " ... " + Station;
        emit titleChanged();

        // Wait if we found the station inside the signal
        stationTimer.start(1000);

        // Clear old data
        currentStationType = "";
        emit stationTypChanged();

        currentLanguageType = "";
        emit languageTypeChanged();

        currentText = "";
        emit textChanged();

        motImage.loadFromData(nullptr, 0);
        emit motChanged(motImage);
    }
}

void CRadioController::nextChannel(bool isWait)
{
    if (isWait) { // It might be a channel, wait 10 seconds
        channelTimer.start(10000);
    }
    else {
        auto Channel = QString::fromStdString(channels.getNextChannel());

        if(!Channel.isEmpty()) {
            setChannel(Channel, true);

            int index = channels.getCurrentIndex() + 1;

            currentTitle = tr("Scanning") + " ... " + Channel
                    + " (" + QString::number(index * 100 / NUMBEROFCHANNELS) + "%)";
            emit titleChanged();

            emit scanProgress(index);
        }
        else {
            stopScan();
        }
    }
}

/********************
 * Controller slots *
 ********************/

void CRadioController::stationTimerTimeout()
{
    if (!my_rx)
        return;

    if (stationListStr.contains(currentStation)) {
        const auto services = my_rx->getServiceList();

        for (const auto& s : services) {
            if (s.serviceLabel.utf8_label() == currentStation.toStdString()) {

                const auto comps = my_rx->getComponents(s);
                for (const auto& sc : comps) {
                    if (sc.transportMode() == TransportMode::Audio && (
                            sc.audioType() == AudioServiceComponentType::DAB ||
                            sc.audioType() == AudioServiceComponentType::DABPlus) ) {
                        const auto& subch = my_rx->getSubchannel(sc);

                        if (not subch.valid()) {
                            return;
                        }

                        // We found the station inside the signal, lets stop the timer
                        stationTimer.stop();

                        std::string dumpFileName;
                        if (commandLineOptions["dumpFileName"] != "") {
                            dumpFileName = commandLineOptions["dumpFileName"].toString().toStdString();
                        }

                        bool success = my_rx->playSingleProgramme(*this, dumpFileName, s);
                        if (!success) {
                            qDebug() << "Selecting service failed";
                        }
                        else {
                            currentTitle = currentStation;
                            emit titleChanged();

                            currentStationType = tr(DABConstants::getProgramTypeName(s.programType));
                            emit stationTypChanged();

                            currentLanguageType = tr(DABConstants::getLanguageName(s.language));
                            emit languageTypeChanged();

                            mBitRate = subch.bitrate();
                            emit bitRateChanged(mBitRate);

                            if (sc.audioType() == AudioServiceComponentType::DABPlus)
                                mIsDAB = false;
                            else
                                mIsDAB = true;
                            emit isDABChanged(mIsDAB);
                        }

                        return;
                    }
                }
            }
        }
    }
}

void CRadioController::channelTimerTimeout(void)
{
    channelTimer.stop();

    if(isChannelScan)
        nextChannel(false);
}

/*****************
 * Backend slots *
 *****************/

void CRadioController::onServiceDetected(uint32_t SId, const std::string& label)
{
    emit ensembleAdded(SId, QString::fromStdString(label));
}

void CRadioController::addtoEnsemble(quint32 SId, const QString &Station)
{
    qDebug() << "RadioController: Found station" <<  Station
             << "(" << qPrintable(QString::number(SId, 16).toUpper()) << ")";

    stationListStr.append(Station);

    if (isChannelScan == true) {
        mStationCount++;
        currentText = tr("Found channels") + ": " + QString::number(mStationCount);
        emit textChanged();
    }

    //	Add new station into list
    if (!stationList.contains(Station, currentChannel)) {
        stationList.append(Station, currentChannel);

        //	Sort stations
        stationList.sort();

        emit stationsChanged(stationList.getList());
        emit foundStation(Station, currentChannel);

        // Save the channels
        stationList.saveStations();
    }
}

void CRadioController::onNewEnsembleName(const std::string& name)
{
    emit ensembleNameUpdated(QString::fromStdString(name));
}

void CRadioController::nameofEnsemble(const QString &Ensemble)
{
    qDebug() << "RadioController: Name of ensemble:" << Ensemble;

    if (currentEnsemble == Ensemble)
        return;
    currentEnsemble = Ensemble;
    emit ensembleChanged();
}


void CRadioController::onDateTimeUpdate(const dab_date_time_t& dateTime)
{
    emit dateTimeUpdated(dateTime);
}

void CRadioController::displayDateTime(const dab_date_time_t& dateTime)
{
    QDate Date;
    QTime Time;

    Time.setHMS(dateTime.hour, dateTime.minutes, dateTime.seconds);
    mCurrentDateTime.setTime(Time);

    Date.setDate(dateTime.year, dateTime.month, dateTime.day);
    mCurrentDateTime.setDate(Date);

    int OffsetFromUtc = dateTime.hourOffset * 3600 +
                        dateTime.minuteOffset * 60;
    mCurrentDateTime.setOffsetFromUtc(OffsetFromUtc);
    mCurrentDateTime.setTimeSpec(Qt::OffsetFromUTC);

    emit dateTimeChanged(mCurrentDateTime);
}

void CRadioController::onFIBDecodeSuccess(bool crcCheckOk, const uint8_t* fib)
{
    (void)fib;
    if (mIsFICCRC == crcCheckOk)
        return;
    mIsFICCRC = crcCheckOk;
    emit isFICCRCChanged(mIsFICCRC);
}

void CRadioController::onNewImpulseResponse(std::vector<float>&& data)
{
    std::lock_guard<std::mutex> lock(impulseResponseBufferMutex);
    impulseResponseBuffer = std::move(data);
}

void CRadioController::onConstellationPoints(std::vector<DSPCOMPLEX>&& data)
{
    std::lock_guard<std::mutex> lock(constellationPointBufferMutex);
    constellationPointBuffer = std::move(data);
}

void CRadioController::onNewNullSymbol(std::vector<DSPCOMPLEX>&& data)
{
    std::lock_guard<std::mutex> lock(nullSymbolBufferMutex);
    nullSymbolBuffer = std::move(data);
}

void CRadioController::onTIIMeasurement(tii_measurement_t&& m)
{
    qDebug().noquote() << "TII comb " << m.comb <<
        " pattern " << m.pattern <<
        " delay " << m.delay_samples <<
        "= " << m.getDelayKm() << " km" <<
        " with error " << m.error;
}

void CRadioController::onMessage(message_level_t level, const std::string& text)
{
    switch (level) {
        case message_level_t::Information:
            emit showInfoMessage(tr(text.c_str()));
            break;
        case message_level_t::Error:
            emit showErrorMessage(tr(text.c_str()));
            break;
    }
}

std::vector<float> CRadioController::getImpulseResponse()
{
    std::lock_guard<std::mutex> lock(impulseResponseBufferMutex);
    auto buf = std::move(impulseResponseBuffer);
    return buf;
}

std::vector<DSPCOMPLEX> CRadioController::getSignalProbe()
{
    int16_t T_u = dabparams.T_u;

    if (device) {
        return device->getSpectrumSamples(T_u);
    }
    else {
        std::vector<DSPCOMPLEX> dummyBuf(static_cast<std::vector<DSPCOMPLEX>::size_type>(T_u));
        return dummyBuf;
    }
}

std::vector<DSPCOMPLEX> CRadioController::getNullSymbol()
{
    std::lock_guard<std::mutex> lock(nullSymbolBufferMutex);
    auto buf = std::move(nullSymbolBuffer);
    return buf;
}

std::vector<DSPCOMPLEX> CRadioController::getConstellationPoint()
{
    std::lock_guard<std::mutex> lock(constellationPointBufferMutex);
    auto buf = std::move(constellationPointBuffer);
    return buf;
}

DABParams& CRadioController::getDABParams()
{
    return dabparams;
}

int CRadioController::getCurrentFrequency()
{
    return currentFrequency;
}

void CRadioController::onSNR(int snr)
{
    if (mSNR == snr)
        return;
    mSNR = snr;
    emit snrChanged(mSNR);
}

void CRadioController::onFrequencyCorrectorChange(int fine, int coarse)
{
    if (mFrequencyCorrection == coarse + fine)
        return;
    mFrequencyCorrection = coarse + fine;
    emit frequencyCorrectionChanged(mFrequencyCorrection);

    if (currentFrequency != 0)
        mFrequencyCorrectionPpm = -1000000.0f * static_cast<float>(mFrequencyCorrection) / static_cast<float>(currentFrequency);
    else
        mFrequencyCorrectionPpm = NAN;
    emit frequencyCorrectionPpmChanged(mFrequencyCorrectionPpm);
}

void CRadioController::onSyncChange(char isSync)
{
    bool sync = (isSync == SYNCED) ? true : false;
    if (mIsSync == sync)
        return;
    mIsSync = sync;
    emit isSyncChanged(mIsSync);
}

void CRadioController::onSignalPresence(bool isSignal)
{
    if (mIsSignal != isSignal) {
        mIsSignal = isSignal;
        emit isSignalChanged(mIsSignal);
    }

    if (isChannelScan)
        emit switchToNextChannel(isSignal);
}

void CRadioController::onNewAudio(std::vector<int16_t>&& audioData, int sampleRate, bool isStereo, const std::string& mode)
{
    audioBuffer.putDataIntoBuffer(audioData.data(), static_cast<int32_t>(audioData.size()));

    if (mAudioSampleRate != sampleRate) {
        qDebug() << "RadioController: Audio sample rate" <<  sampleRate << "kHz, mode=" <<
            QString::fromStdString(mode);
        mAudioSampleRate = sampleRate;
        emit audioSampleRateChanged(mAudioSampleRate);

        audio.setRate(sampleRate);
    }

    if (mIsStereo != isStereo) {
        mIsStereo = isStereo;
        emit isStereoChanged(mIsStereo);
    }
}

void CRadioController::onFrameErrors(int frameErrors)
{
    if (mFrameErrors == frameErrors)
        return;
    mFrameErrors = frameErrors;
    emit frameErrorsChanged(mFrameErrors);
}

void CRadioController::onRsErrors(int rsErrors)
{
    if (mRSErrors == rsErrors)
        return;
    mRSErrors = rsErrors;
    emit rsErrorsChanged(mRSErrors);
}

void CRadioController::onAacErrors(int aacErrors)
{
    if (mAACErrors == aacErrors)
        return;
    mAACErrors = aacErrors;
    emit aacErrorsChanged(mAACErrors);
}

void CRadioController::onNewDynamicLabel(const std::string& label)
{
    auto qlabel = QString::fromUtf8(label.c_str());
    if (this->currentText != qlabel) {
        this->currentText = qlabel;
        emit textChanged();
    }
}

void CRadioController::onMOT(const std::vector<uint8_t>& Data, int subtype)
{
    QByteArray qdata(reinterpret_cast<const char*>(Data.data()), static_cast<int>(Data.size()));

    motImage.loadFromData(qdata, subtype == 0 ? "GIF" : subtype == 1 ? "JPEG" : subtype == 2 ? "BMP" : "PNG");

    emit motChanged(motImage);
}

void CRadioController::onPADLengthError(size_t announced_xpad_len, size_t xpad_len)
{
    qDebug() << "X-PAD length mismatch, expected:" << announced_xpad_len << " effective:" << xpad_len;
}

void CRadioController::setAutoPlay(QString Channel, QString Station)
{
    isAutoPlay = true;
    autoChannel = Channel;
    autoStation = Station;
}

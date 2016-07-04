#include "esp8266.h"

#include <iostream>
#include <map>
#include <memory>
#include <string>

#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QtDebug>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <common/util/error_codes.h>
#include <common/util/statusor.h>

#include "config.h"
#include "esp_flasher_client.h"
#include "esp_rom_client.h"
#include "fs.h"
#include "serial.h"
#include "status_qt.h"

#if (QT_VERSION < QT_VERSION_CHECK(5, 5, 0))
#define qInfo qWarning
#endif

namespace ESP8266 {

namespace {

const char kFlashParamsOption[] = "esp8266-flash-params";
const char kFlashSizeOption[] = "esp8266-flash-size";
const char kFlashingDataPortOption[] = "esp8266-flashing-data-port";
const char kSPIFFSOffsetOption[] = "esp8266-spiffs-offset";
const char kDefaultSPIFFSOffset[] = "0xec000";
const char kSPIFFSSizeOption[] = "esp8266-spiffs-size";
const char kDefaultSPIFFSSize[] = "65536";
const char kNoMinimizeWritesOption[] = "esp8266-no-minimize-writes";

const int kDefaultROMBaudRate = 115200;
const int kDefaultFlashBaudRate = 230400;
/* Last 16K of flash are reserved for system params. */
const quint32 kSystemParamsAreaSize = 16 * 1024;

#define FLASHING_MSG                                             \
  "Failed to talk to bootloader. See <a "                        \
  "href=\"https://github.com/cesanta/mongoose-iot/blob/master/"  \
  "fw/platforms/esp8266/flashing.md\">wiring instructions</a>. " \
  "Alternatively, put the device into flashing mode "            \
  "(GPIO0 = 0, reset) manually and "                             \
  "retry now."

class FlasherImpl : public Flasher {
  Q_OBJECT
 public:
  FlasherImpl(QSerialPort *port, Prompter *prompter)
      : port_(port), prompter_(prompter) {
  }

  util::Status setOption(const QString &name, const QVariant &value) override {
    if (name == kFlashSizeOption) {
      auto res = parseSize(value);
      if (res.ok()) flashSize_ = res.ValueOrDie();
      return res.status();
    } else if (name == kMergeFSOption) {
      if (value.type() != QVariant::Bool) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be boolean");
      }
      merge_flash_filesystem_ = value.toBool();
      return util::Status::OK;
    } else if (name == kFlashParamsOption) {
      if (value.type() == QVariant::String) {
        auto r = flashParamsFromString(value.toString());
        if (!r.ok()) {
          return r.status();
        }
        override_flash_params_ = r.ValueOrDie();
      } else if (value.canConvert<int>()) {
        override_flash_params_ = value.toInt();
      } else {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be a number or a string");
      }
      return util::Status::OK;
    } else if (name == kFlashingDataPortOption) {
      if (value.type() != QVariant::String) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be a string");
      }
      flashing_port_name_ = value.toString();
      return util::Status::OK;
    } else if (name == kFlashBaudRateOption) {
      if (value.type() != QVariant::Int) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be a positive integer");
      }
      flashing_speed_ = value.toInt();
      if (flashing_speed_ <= 0) {
        flashing_speed_ = kDefaultFlashBaudRate;
      }
      return util::Status::OK;
    } else if (name == kDumpFSOption) {
      if (value.type() != QVariant::String) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be a string");
      }
      fs_dump_filename_ = value.toString();
      return util::Status::OK;
    } else if (name == kSPIFFSOffsetOption) {
      if (value.type() != QVariant::Int || value.toInt() <= 0) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be a positive integer");
      }
      spiffs_offset_ = value.toInt();
      return util::Status::OK;
    } else if (name == kSPIFFSSizeOption) {
      if (value.type() != QVariant::Int || value.toInt() <= 0) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be a positive integer");
      }
      spiffs_size_ = value.toInt();
      return util::Status::OK;
    } else if (name == kNoMinimizeWritesOption) {
      if (value.type() != QVariant::Bool) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "value must be boolean");
      }
      minimize_writes_ = !value.toBool();
      return util::Status::OK;
    } else {
      return util::Status(util::error::INVALID_ARGUMENT, "unknown option");
    }
  }

  util::Status setOptionsFromConfig(const Config &config) override {
    util::Status r;

    QStringList boolOpts({kMergeFSOption, kNoMinimizeWritesOption});
    for (const auto &opt : boolOpts) {
      auto s = setOption(opt, config.isSet(opt));
      if (!s.ok()) {
        return util::Status(
            s.error_code(),
            (opt + ": " + s.error_message().c_str()).toStdString());
      }
    }

    QStringList stringOpts({kFlashSizeOption, kFlashParamsOption,
                            kFlashingDataPortOption, kDumpFSOption});
    for (const auto &opt : stringOpts) {
      // XXX: currently there's no way to "unset" a string option.
      if (config.isSet(opt)) {
        auto s = setOption(opt, config.value(opt));
        if (!s.ok()) {
          return util::Status(
              s.error_code(),
              (opt + ": " + s.error_message().c_str()).toStdString());
        }
      }
    }

    QStringList intOpts(
        {kFlashBaudRateOption, kSPIFFSOffsetOption, kSPIFFSSizeOption});
    for (const auto &opt : intOpts) {
      bool ok;
      int value = config.value(opt).toInt(&ok, 0);
      if (!ok) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            (opt + ": Invalid numeric value.").toStdString());
      }
      auto s = setOption(opt, value);
      if (!s.ok()) {
        return util::Status(
            s.error_code(),
            (opt + ": " + s.error_message().c_str()).toStdString());
      }
    }
    return util::Status::OK;
  }

  util::Status setFirmware(FirmwareBundle *fw) override {
    QMutexLocker lock(&lock_);
    for (const auto &p : fw->parts()) {
      if (!p.attrs["addr"].isValid()) {
        return QS(util::error::INVALID_ARGUMENT,
                  QObject::tr("part %1 has no address specified").arg(p.name));
      }
      bool ok;
      const quint32 addr = p.attrs["addr"].toUInt(&ok);
      if (!ok) {
        return QS(util::error::INVALID_ARGUMENT,
                  QObject::tr("part %1 has invalid address specified (%2)")
                      .arg(p.name)
                      .arg(p.attrs["addr"].toString()));
      }
      const auto data = fw->getPartSource(p.name);
      if (!data.ok()) return data.status();
      qInfo() << p.name << ":" << data.ValueOrDie().length() << "@" << hex
              << showbase << addr;
      blobs_[addr] = data.ValueOrDie();
    }
    return util::Status::OK;
  }

  int totalBytes() const override {
    QMutexLocker lock(&lock_);
    int r = 0;
    for (const auto &bytes : blobs_.values()) {
      r += bytes.length();
    }
    // Add FS once again for reading.
    if (merge_flash_filesystem_ && blobs_.contains(spiffs_offset_)) {
      r += blobs_[spiffs_offset_].length();
    }
    return r;
  }

  util::StatusOr<std::unique_ptr<QSerialPort>> getFlashingDataPort() {
    std::unique_ptr<QSerialPort> second_port;
    if (!flashing_port_name_.isEmpty()) {
      const auto &ports = QSerialPortInfo::availablePorts();
      QSerialPortInfo info;
      bool found = false;
      for (const auto &port : ports) {
        if (port.systemLocation() != flashing_port_name_) {
          continue;
        }
        info = port;
        found = true;
        break;
      }
      if (!found) {
        return util::Status(
            util::error::NOT_FOUND,
            tr("Port %1 not found").arg(flashing_port_name_).toStdString());
      }
      auto serial = connectSerial(info, kDefaultROMBaudRate);
      if (!serial.ok()) {
        return util::Status(
            util::error::UNKNOWN,
            tr("Failed to open %1: %2")
                .arg(flashing_port_name_)
                .arg(QString::fromStdString(serial.status().ToString()))
                .toStdString());
      }
      second_port.reset(serial.ValueOrDie());
    }

    return std::move(second_port);
  }

  void run() override {
    QMutexLocker lock(&lock_);

    util::Status st = runLocked();
    if (!st.ok()) {
      emit done(QString::fromStdString(st.error_message()), false);
      return;
    }
    emit done(tr("All done!"), true);
  }

 private:
  util::Status runLocked() {
    if (blobs_.empty()) {
      return QS(util::error::FAILED_PRECONDITION, tr("No firmware loaded"));
    }
    progress_ = 0;
    emit progress(progress_);

    auto fdps = getFlashingDataPort();
    if (!fdps.ok()) {
      return QSP("failed to open flashing data port", fdps.status());
    }

    ESPROMClient rom(port_,
                     fdps.ValueOrDie().get() ? fdps.ValueOrDie().get() : port_);

    emit statusMessage("Connecting to ROM...", true);

    util::Status st;
    while (true) {
      st = rom.connect();
      if (st.ok()) break;
      qCritical() << st;
      QString msg = tr(FLASHING_MSG "\n\nError: %1")
                        .arg(QString::fromUtf8(st.ToString().c_str()));
      int answer =
          prompter_->Prompt(msg, {{tr("Retry"), Prompter::ButtonRole::No},
                                  {tr("Cancel"), Prompter::ButtonRole::Yes}});
      if (answer == 1) {
        return util::Status(util::error::UNAVAILABLE,
                            "Failed to talk to bootloader.");
      }
    }

    emit statusMessage("Running flasher...", true);

    ESPFlasherClient flasher_client(&rom);

    st = flasher_client.connect(flashing_speed_);
    if (!st.ok()) {
      return QSP("Failed to run and communicate with flasher stub", st);
    }

    if (override_flash_params_ >= 0) {
      // This really can't go wrong, we parsed the params.
      flashSize_ = flashSizeFromParams(override_flash_params_).ValueOrDie();
    } else if (flashSize_ == 0) {
      qInfo() << "Detecting flash size...";
      auto flashChipIDRes = flasher_client.getFlashChipID();
      if (flashChipIDRes.ok()) {
        quint32 mfg = (flashChipIDRes.ValueOrDie() & 0xff000000) >> 24;
        quint32 type = (flashChipIDRes.ValueOrDie() & 0x00ff0000) >> 16;
        quint32 capacity = (flashChipIDRes.ValueOrDie() & 0x0000ff00) >> 8;
        qInfo() << "Flash chip ID:" << hex << showbase << mfg << type
                << capacity;
        if (mfg != 0 && capacity >= 0x13 && capacity < 0x20) {
          // Capacity is the power of two.
          flashSize_ = 1 << capacity;
        }
      }
      if (flashSize_ == 0) {
        qWarning()
            << "Failed to detect flash size:" << flashChipIDRes.status()
            << ", defaulting 512K. You may want to specify size explicitly "
               "using --flash-size.";
        flashSize_ = 512 * 1024;  // A safe default.
      }
    }
    qInfo() << "Flash size:" << flashSize_;

    st = sanityCheckImages(blobs_, flashSize_, flasher_client.kFlashSectorSize);
    if (!st.ok()) return st;

    if (blobs_.contains(0) && blobs_[0].length() >= 4) {
      int flashParams = 0;
      if (override_flash_params_ >= 0) {
        flashParams = override_flash_params_;
      } else {
        // We don't have constants for larger flash sizes.
        if (flashSize_ > 4194304) flashSize_ = 4194304;
        // We use detected size + DIO @ 40MHz which should be a safe default.
        // Advanced users wishing to use other modes and freqs can override.
        flashParams =
            flashParamsFromString(
                tr("dio,%1m,40m").arg(flashSize_ * 8 / 1048576)).ValueOrDie();
      }
      blobs_[0][2] = (flashParams >> 8) & 0xff;
      blobs_[0][3] = flashParams & 0xff;
      emit statusMessage(
          tr("Setting flash params to 0x%1").arg(flashParams, 0, 16), true);
    }

    qInfo() << QString("SPIFFS params: %1 @ 0x%2")
                   .arg(spiffs_size_)
                   .arg(spiffs_offset_, 0, 16)
                   .toUtf8();
    if (merge_flash_filesystem_ && blobs_.contains(spiffs_offset_)) {
      auto res = mergeFlashLocked(&flasher_client);
      if (res.ok()) {
        if (res.ValueOrDie().size() > 0) {
          blobs_[spiffs_offset_] = res.ValueOrDie();
        } else {
          blobs_.remove(spiffs_offset_);
        }
        emit statusMessage(tr("Merged flash content"), true);
      } else {
        emit statusMessage(tr("Failed to merge flash content: %1")
                               .arg(res.status().ToString().c_str()),
                           true);
      }
    } else if (merge_flash_filesystem_) {
      qInfo() << "No SPIFFS image in new firmware";
    }

    auto flashImages =
        minimize_writes_ ? dedupImages(&flasher_client, blobs_) : blobs_;

    for (ulong image_addr : flashImages.keys()) {
      QByteArray data = flashImages[image_addr];
      emit progress(progress_);
      int origLength = data.length();

      if (data.length() % flasher_client.kFlashSectorSize != 0) {
        quint32 padLen = flasher_client.kFlashSectorSize -
                         (data.length() % flasher_client.kFlashSectorSize);
        data.reserve(data.length() + padLen);
        while (padLen-- > 0) data.append('\x00');
      }

      emit statusMessage(
          tr("Writing %1 @ 0x%2...").arg(data.length()).arg(image_addr, 0, 16),
          true);
      connect(
          &flasher_client, &ESPFlasherClient::progress,
          [this, origLength](int bytesWritten) {
            emit progress(this->progress_ + std::min(bytesWritten, origLength));
          });
      st = flasher_client.write(image_addr, data, true /* erase */);
      disconnect(&flasher_client, &ESPFlasherClient::progress, 0, 0);
      if (!st.ok()) {
        return QS(util::error::UNAVAILABLE,
                  tr("failed to flash image at 0x%1: %2")
                      .arg(image_addr, 0, 16)
                      .arg(st.ToString().c_str()));
      }
      progress_ += origLength;
    }

    st = verifyImages(&flasher_client, blobs_);
    if (!st.ok()) return QSP("verification failed", st);

    emit statusMessage(tr("Flashing successful, booting firmare..."), true);

    // So, this is a bit tricky. Rebooting ESP8266 "properly" from software
    // seems to be impossible due to GPIO strapping: at this point we have
    // STRAPPING_GPIO0 = 0 and as far as we are aware it's not possible to
    // perform a reset that will cause strapping bits to be re-initialized.
    // Jumping to ResetVector or perforing RTC reset (bit 31 in RTC_CTL)
    // simply gets us back into boot loader.
    // flasher_client performs a "soft" reboot, which simply jumps to the
    // routine that loads. This will work even if RTS and DTR are not connected,
    // but the side effect is that firmware will not be able to reboot properly.
    // So, what we do is we do both: tell the flasher to boot firmware *and*
    // tickle RTS as well. Thus, setups that have control lines connected will
    // get a "proper" hardware reset, while setups that don't will still work.
    st = flasher_client.bootFirmware();  // Jumps to flash loader routine.
    rom.rebootIntoFirmware();            // Uses RTS.
    return st;
  }

  util::Status sanityCheckImages(const QMap<ulong, QByteArray> &images,
                                 quint32 flashSize, quint32 flashSectorSize) {
    const auto keys = images.keys();
    for (int i = 0; i < keys.length(); i++) {
      const quint32 imageBegin = keys[i];
      const QByteArray &image = images[imageBegin];
      const quint32 imageEnd = imageBegin + image.length();
      if (imageBegin >= flashSize || imageEnd > flashSize) {
        return QS(util::error::INVALID_ARGUMENT,
                  tr("Image %1 @ 0x%2 will not fit in flash (size %3)")
                      .arg(image.length())
                      .arg(imageBegin, 0, 16)
                      .arg(flashSize));
      }
      if (imageBegin % flashSectorSize != 0) {
        return QS(util::error::INVALID_ARGUMENT,
                  tr("Image starting address (0x%1) is not on flash sector "
                     "boundary (sector size %2)")
                      .arg(imageBegin, 0, 16)
                      .arg(flashSectorSize));
      }
      if (imageBegin == 0 && image.length() >= 1) {
        if (image[0] != (char) 0xE9) {
          return QS(util::error::INVALID_ARGUMENT,
                    tr("Invalid magic byte in the first image"));
        }
      }
      const quint32 systemParamsBegin = flashSize - kSystemParamsAreaSize;
      const quint32 systemParamsEnd = flashSize;
      if (imageBegin < systemParamsEnd && imageEnd > systemParamsBegin) {
        return QS(util::error::INVALID_ARGUMENT,
                  tr("Image 0x%1 overlaps with system params area (%2 @ 0x%3)")
                      .arg(imageBegin, 0, 16)
                      .arg(kSystemParamsAreaSize)
                      .arg(systemParamsBegin, 0, 16));
      }
      if (i > 0) {
        const quint32 prevImageBegin = keys[i - 1];
        const quint32 prevImageEnd = keys[i - 1] + images[keys[i - 1]].length();
        // We traverse the list in order, so a simple check will suffice.
        if (prevImageEnd > imageBegin) {
          return QS(util::error::INVALID_ARGUMENT,
                    tr("Images at offsets 0x%1 and 0x%2 overlap.")
                        .arg(prevImageBegin, 0, 16)
                        .arg(imageBegin, 0, 16));
        }
      }
    }
    return util::Status::OK;
  }

  // mergeFlashLocked reads the spiffs filesystem from the device
  // and mounts it in memory. Then it overwrites the files that are
  // present in the software update but it leaves the existing ones.
  // The idea is that the filesystem is mostly managed by the user
  // or by the software update utility, while the core system uploaded by
  // the flasher should only upload a few core files.
  util::StatusOr<QByteArray> mergeFlashLocked(ESPFlasherClient *fc) {
    emit statusMessage(tr("Reading file system image (%1 @ %2)...")
                           .arg(spiffs_size_)
                           .arg(spiffs_offset_, 0, 16),
                       true);
    connect(fc, &ESPFlasherClient::progress, [this](int bytesRead) {
      emit progress(this->progress_ + bytesRead);
    });
    auto dev_fs = fc->read(spiffs_offset_, spiffs_size_);
    disconnect(fc, &ESPFlasherClient::progress, 0, 0);
    if (!dev_fs.ok()) {
      return dev_fs.status();
    }
    progress_ += spiffs_size_;
    if (!fs_dump_filename_.isEmpty()) {
      QFile f(fs_dump_filename_);
      if (f.open(QIODevice::WriteOnly)) {
        f.write(dev_fs.ValueOrDie());
      } else {
        qCritical() << "Failed to open" << fs_dump_filename_ << ":"
                    << f.errorString();
      }
    }
    auto merged = mergeFilesystems(dev_fs.ValueOrDie(), blobs_[spiffs_offset_]);
    if (!merged.ok()) {
      QString msg = tr("Failed to merge file system: ") +
                    QString(merged.status().ToString().c_str()) +
                    tr("\nWhat should we do?");
      int answer =
          prompter_->Prompt(msg, {{tr("Cancel"), Prompter::ButtonRole::Reject},
                                  {tr("Write new"), Prompter::ButtonRole::Yes},
                                  {tr("Keep old"), Prompter::ButtonRole::No}});
      qCritical() << msg << "->" << answer;
      switch (answer) {
        case 0:
          return merged.status();
        case 1:
          return blobs_[spiffs_offset_];
        case 2:
          return QByteArray();
      }
    }
    return merged;
  }

  QMap<ulong, QByteArray> dedupImages(ESPFlasherClient *fc,
                                      QMap<ulong, QByteArray> images) {
    QMap<ulong, QByteArray> result;
    emit statusMessage("Checking existing contents...", true);
    for (auto im = images.constBegin(); im != images.constEnd(); im++) {
      const ulong addr = im.key();
      const QByteArray &data = im.value();
      emit statusMessage(
          tr("Checksumming %1 @ 0x%2...").arg(data.length()).arg(addr, 0, 16),
          true);
      ESPFlasherClient::DigestResult digests;
      auto dr = fc->digest(addr, data.length(), fc->kFlashSectorSize);
      QMap<ulong, QByteArray> newImages;
      if (!dr.ok()) {
        qWarning() << "Error computing digest:" << dr.status();
        return images;
      }
      QMap<ulong, QByteArray> newImage;
      digests = dr.ValueOrDie();
      int numBlocks =
          (data.length() + fc->kFlashSectorSize - 1) / fc->kFlashSectorSize;
      quint32 newAddr = addr, newLen = 0;
      quint32 newImageSize = 0;
      for (int i = 0; i < numBlocks; i++) {
        int offset = i * fc->kFlashSectorSize;
        int len = fc->kFlashSectorSize;
        if (len > data.length() - offset) len = data.length() - offset;
        const QByteArray &hash = QCryptographicHash::hash(
            data.mid(offset, len), QCryptographicHash::Md5);
        qDebug() << i << offset << len << hash.toHex()
                 << digests.blockDigests[i].toHex();
        if (hash == digests.blockDigests[i]) {
          // This block is the same, skip it. Flush previous image, if any.
          if (newLen > 0) {
            newImage[newAddr] = data.mid(newAddr - addr, newLen);
            newLen = 0;
            qDebug() << "New image:" << newImage[newAddr].length() << "@" << hex
                     << showbase << newAddr;
          }
          progress_ += len;
          emit progress(progress_);
        } else {
          // This block is different. Start new or extend existing image.
          if (newLen == 0) {
            newAddr = addr + i * fc->kFlashSectorSize;
          }
          newLen += len;
          newImageSize += len;
        }
      }
      if (newLen > 0) {
        newImage[newAddr] = data.mid(newAddr - addr, newLen);
        qDebug() << "New image:" << newImage[newAddr].length() << "@" << hex
                 << showbase << newAddr;
      }
      qInfo() << hex << showbase << addr << "was" << dec << data.length()
              << "now" << newImageSize << "diff"
              << (data.length() - newImageSize);
      // There's a price for fragmenting a large the image: individual sectors
      // is slower than erasing a whole block. So unless the difference is
      // substantial, don't bother.
      if (data.length() - newImageSize >= ESPFlasherClient::kFlashBlockSize) {
        result.unite(newImage);  // There are no dup keys, so unite is ok.
      } else {
        result[addr] = data;
      }
    }
    qDebug() << "After deduping:" << result.size() << "images";
    return result;
  }

  util::Status verifyImages(ESPFlasherClient *fc,
                            QMap<ulong, QByteArray> images) {
    for (auto im = images.constBegin(); im != images.constEnd(); im++) {
      const ulong addr = im.key();
      const QByteArray &data = im.value();
      emit statusMessage(tr("Verifying image at 0x%1...").arg(addr, 0, 16),
                         true);
      auto dr = fc->digest(addr, data.length(), 0 /* no block sums */);
      if (!dr.ok()) {
        return QSP(tr("failed to compute digest of 0x%1").arg(addr, 0, 16),
                   dr.status());
      }
      ESPFlasherClient::DigestResult digests = dr.ValueOrDie();
      const QByteArray &hash =
          QCryptographicHash::hash(data, QCryptographicHash::Md5);
      qDebug() << hex << showbase << addr << data.length() << hash.toHex()
               << digests.digest.toHex();
      if (hash != digests.digest) {
        return QS(util::error::DATA_LOSS,
                  tr("Hash mismatch for image 0x%1").arg(addr, 0, 16));
      }
    }
    return util::Status::OK;
  }

  QSerialPort *port_;
  Prompter *prompter_;

  mutable QMutex lock_;
  QMap<ulong, QByteArray> blobs_;
  std::unique_ptr<ESPROMClient> rom_;
  int progress_ = 0;
  quint32 flashSize_ = 0;
  qint32 override_flash_params_ = -1;
  bool merge_flash_filesystem_ = false;
  QString flashing_port_name_;
  int flashing_speed_ = kDefaultFlashBaudRate;
  bool minimize_writes_ = true;
  ulong spiffs_size_ = 0;
  ulong spiffs_offset_ = 0;
  QString fs_dump_filename_;
};

class ESP8266HAL : public HAL {
 public:
  ESP8266HAL(QSerialPort *port) : port_(port) {
  }

  util::Status probe() const override {
    ESPROMClient rom(port_, port_);

    if (!rom.connect().ok()) {
      return QS(util::error::UNAVAILABLE, FLASHING_MSG);
    }

    auto mac = rom.readMAC();
    if (!mac.ok()) {
      qDebug() << "Error reading MAC address:" << mac.status();
      return mac.status();
    }
    qInfo() << "MAC address: " << mac.ValueOrDie().toHex();

    rom.softReset();

    return util::Status::OK;
  }

  std::unique_ptr<Flasher> flasher(Prompter *prompter) const override {
    return std::move(
        std::unique_ptr<Flasher>(new FlasherImpl(port_, prompter)));
  }

  std::string name() const override {
    return "ESP8266";
  }

  util::Status reboot() override {
    // TODO(rojer): Bring flashing data port setting here somehow.
    ESPROMClient rom(port_, port_);
    // To make sure we actually control things, connect to ROM first.
    util::Status st = rom.connect();
    if (!st.ok()) return QSP("failed to communicate to ROM", st);
    return rom.rebootIntoFirmware();
  }

 private:
  QSerialPort *port_;
};

}  // namespace

std::unique_ptr<::HAL> HAL(QSerialPort *port) {
  return std::move(std::unique_ptr<::HAL>(new ESP8266HAL(port)));
}

namespace {

using std::map;
using std::string;

const map<string, int> flashMode = {
    {"qio", 0}, {"qout", 1}, {"dio", 2}, {"dout", 3},
};

const map<string, int> flashSize = {
    {"4m", 0},
    {"2m", 1},
    {"8m", 2},
    {"16m", 3},
    {"32m", 4},
    {"16m-c1", 5},
    {"32m-c1", 6},
    {"32m-c2", 7},
};

const map<int, int> flashSizeById = {{0, 524288},
                                     {1, 262144},
                                     {2, 1048576},
                                     {3, 2097152},
                                     {4, 4194304},
                                     {5, 2097152},
                                     {6, 4194304},
                                     {7, 4194304}};

const map<string, int> flashFreq = {
    {"40m", 0}, {"26m", 1}, {"20m", 2}, {"80m", 0xf},
};
}

util::StatusOr<int> flashParamsFromString(const QString &s) {
  QStringList parts = s.split(',');
  switch (parts.size()) {
    case 1: {  // number
      bool ok = false;
      int r = s.toInt(&ok, 0);
      if (!ok) {
        return util::Status(util::error::INVALID_ARGUMENT, "invalid number");
      }
      return r & 0xffff;
    }
    case 3:  // string
      if (flashMode.find(parts[0].toStdString()) == flashMode.end()) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "invalid flash mode");
      }
      if (flashSize.find(parts[1].toStdString()) == flashSize.end()) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "invalid flash size");
      }
      if (flashFreq.find(parts[2].toStdString()) == flashFreq.end()) {
        return util::Status(util::error::INVALID_ARGUMENT,
                            "invalid flash frequency");
      }
      return (flashMode.find(parts[0].toStdString())->second << 8) |
             (flashSize.find(parts[1].toStdString())->second << 4) |
             (flashFreq.find(parts[2].toStdString())->second);
    default:
      return util::Status(
          util::error::INVALID_ARGUMENT,
          "must be either a number or a comma-separated list of three items");
  }
}

util::StatusOr<int> flashSizeFromParams(int flashParams) {
  int flashSizeId = (flashParams & 0xff) >> 4;
  if (flashSizeById.find(flashSizeId) == flashSizeById.end()) {
    return util::Status(util::error::INVALID_ARGUMENT, "invalid flash size id");
  }
  return flashSizeById.at(flashSizeId);
}

void addOptions(Config *config) {
  // QCommandLineOption supports C++11-style initialization only since Qt 5.4.
  QList<QCommandLineOption> opts;
  opts.append(QCommandLineOption(
      kFlashSizeOption,
      "Size of the flash chip. If not specified, will auto-detect. Size can be "
      "specified as an integer number of bytes and larger units of {k,m}bits or"
      " {K,M}bytes. 1M = 1024K = 8m = 8192k = 1048576 bytes.",
      "<size>[KkMm]"));
  opts.append(QCommandLineOption(
      kFlashParamsOption,
      "Override params bytes read from existing firmware. Either a "
      "comma-separated string or a number. First component of the string is "
      "the flash mode, must be one of: qio (default), qout, dio, dout. "
      "Second component is flash size, value values: 2m, 4m (default), 8m, "
      "16m, 32m, 16m-c1, 32m-c1, 32m-c2. Third one is flash frequency, valid "
      "values: 40m (default), 26m, 20m, 80m. If it's a number, only 2 lowest "
      "bytes from it will be written in the header of section 0x0000 in "
      "big-endian byte order (i.e. high byte is put at offset 2, low byte at "
      "offset 3).",
      "params"));
  opts.append(QCommandLineOption(
      kFlashingDataPortOption,
      "If set, communication with ROM will be performed using another serial "
      "port. DTR/RTS signals for rebooting and console will still use the "
      "main port.",
      "port"));
  opts.append(QCommandLineOption(
      kSPIFFSOffsetOption, "Location of the SPIFFS filesystem block in flash.",
      "offset", kDefaultSPIFFSOffset));
  opts.append(QCommandLineOption(kSPIFFSSizeOption,
                                 "Size of the SPIFFS region in flash.", "size",
                                 kDefaultSPIFFSSize));
  opts.append(QCommandLineOption(
      kNoMinimizeWritesOption,
      "If set, no attempt will be made to minimize the number of blocks to "
      "write by comparing current contents with the images being written."));
  config->addOptions(opts);
}

QByteArray makeIDBlock(const QString &domain) {
  QByteArray data = randomDeviceID(domain);
  QByteArray r = QCryptographicHash::hash(data, QCryptographicHash::Sha1)
                     .append(data)
                     .append("\0", 1);
  return r;
}

}  // namespace ESP8266

#include "esp8266.moc"

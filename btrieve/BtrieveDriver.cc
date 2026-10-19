#include "BtrieveDriver.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <filesystem>
#include <ctime>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "BtrieveDatabase.h"
#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace btrieve {
BtrieveDriver::~BtrieveDriver() { close(); }

static inline bool fileExists(const wchar_t *filename,
                              int64_t &fileModificationNanos) {
#ifdef WIN32
  WIN32_FILE_ATTRIBUTE_DATA fileAttributeData;

  if (GetFileAttributesEx(filename, GetFileExInfoStandard,
                          &fileAttributeData) == 0) {
    return false;
  }

  fileModificationNanos =
      *reinterpret_cast<int64_t *>(&fileAttributeData.ftLastWriteTime);
  return true;
#else
  struct stat stbuf;
  if (stat(toStdString(filename).c_str(), &stbuf)) {
    return false;
  }

#ifdef __APPLE__
  // macOS's struct stat has no st_mtim (POSIX.1-2008); the equivalent
  // timespec field is named st_mtimespec there.
  fileModificationNanos = (stbuf.st_mtimespec.tv_sec * 1000000000l) +
                          stbuf.st_mtimespec.tv_nsec;
#else
  fileModificationNanos =
      (stbuf.st_mtim.tv_sec * 1000000000l) + stbuf.st_mtim.tv_nsec;
#endif
  return true;
#endif
}

static std::basic_string<wchar_t> toUpper(
    const std::basic_string<wchar_t> &value) {
  std::basic_string<wchar_t> ret(value);
  for (size_t i = 0; i < ret.size(); ++i) {
    ret[i] = toupper(ret[i]);
  }
  return ret;
}

#ifdef _WIN32
#define unlink _wunlink
#endif

// FNV-1a over the whole file: cheap, and only computed when converting or
// when a file's size or timestamp no longer matches what was recorded.
static SourceFingerprint fingerprintOf(const wchar_t *fileName, uint64_t size,
                                       int64_t modificationTime) {
  SourceFingerprint fingerprint;
  fingerprint.size = size;
  fingerprint.modificationTime = modificationTime;
  fingerprint.hash = 14695981039346656037ull;

  std::ifstream file(std::filesystem::path(fileName), std::ios::binary);
  std::vector<char> buffer(1 << 16);
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const size_t got = static_cast<size_t>(file.gcount());
    for (size_t i = 0; i < got; ++i) {
      fingerprint.hash ^= static_cast<uint8_t>(buffer[i]);
      fingerprint.hash *= 1099511628211ull;
    }
  }
  return fingerprint;
}

static std::basic_string<wchar_t> toLower(
    const std::basic_string<wchar_t> &value) {
  std::basic_string<wchar_t> ret(value);
  for (size_t i = 0; i < ret.size(); ++i) {
    ret[i] = tolower(ret[i]);
  }
  return ret;
}

std::filesystem::path BtrieveDriver::sqlPathFor(
    const std::filesystem::path &btrieveFile,
    const std::wstring &sqlExtension) {
  const std::wstring extension = toLower(btrieveFile.extension().wstring());
  std::filesystem::path sqlPath = btrieveFile;
  // .DAT as always; the database's own extension too, since callers may name
  // an already-converted database directly
  if (extension.empty() || extension == L".dat" ||
      extension == L"." + toLower(sqlExtension)) {
    return sqlPath.replace_extension(sqlExtension);
  }
  sqlPath += L".";
  sqlPath += sqlExtension;
  return sqlPath;
}

// Moves a .db (and its -wal/-shm, which may hold committed data) aside as
// .db.bak instead of deleting it, replacing any previous backup. Returns false
// if the .db couldn't be moved (e.g. it's open elsewhere), leaving it as it
// was. backupName receives the backup's file name.
static bool backupDatabase(const std::filesystem::path &dbPath,
                           std::string &backupName) {
  std::filesystem::path backup = dbPath;
  backup += ".bak";
  backupName = backup.filename().string();

  std::error_code error;
  for (const char *suffix : {"", "-wal", "-shm"}) {
    std::filesystem::path from = dbPath, to = backup;
    from += suffix;
    to += suffix;
    if (!std::filesystem::exists(from, error)) {
      continue;
    }
    std::filesystem::remove(to, error);
    std::filesystem::rename(from, to, error);
    if (error && *suffix == 0) {
      return false;  // the database itself stayed put
    }
  }
  return true;
}

static bool readMarker(const std::filesystem::path &file,
                       BtrieveDriver::Marker &marker) {
  std::ifstream in(file, std::ios::binary);
  char buffer[1024] = {};
  in.read(buffer, sizeof(buffer) - 1);
  const std::string text(buffer, static_cast<size_t>(in.gcount()));
  if (text.rfind(BtrieveDriver::markerMagic(), 0) != 0) {
    return false;
  }
  size_t lineStart = 0;
  while (lineStart < text.size()) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = text.size();
    }
    std::string line = text.substr(lineStart, lineEnd - lineStart);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("db=", 0) == 0) {
      marker.database = line.substr(3);
    } else if (line.rfind("id=", 0) == 0) {
      marker.id = line.substr(3);
    }
    lineStart = lineEnd + 1;
  }
  return !marker.database.empty() && !marker.id.empty();
}

std::string BtrieveDriver::newDatabaseId() {
  std::random_device random;
  char id[33];
  snprintf(id, sizeof(id), "%08x%08x%08x%08x", random(), random(), random(),
           random());
  return id;
}

bool BtrieveDriver::writeMarker(const std::filesystem::path &markerFile,
                                const std::filesystem::path &database,
                                const std::string &id) {
  std::ofstream out(markerFile, std::ios::binary | std::ios::trunc);
  out << markerMagic()
      << " - this file's data is in the SQLite database named below.\r\n"
         "Copying, renaming or deleting this file copies, renames or deletes "
         "that data.\r\n"
      << "db=" << database.filename().string() << "\r\n"
      << "id=" << id << "\r\n";
  return static_cast<bool>(out);
}

// Moves the original Btrieve file into wbtrv32-originals beside it, so a
// marker can take its place. An older copy already there is kept, renamed
// with a time stamp.
static bool moveOriginalAside(const std::filesystem::path &file,
                              std::string &movedTo) {
  std::error_code error;
  const std::filesystem::path folder =
      file.parent_path() / "wbtrv32-originals";
  std::filesystem::create_directories(folder, error);
  const std::filesystem::path target = folder / file.filename();
  if (std::filesystem::exists(target, error)) {
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    strftime(stamp, sizeof(stamp), ".%Y%m%d%H%M%S", &local);
    std::filesystem::path older = target;
    older += stamp;
    std::filesystem::rename(target, older, error);
  }
  std::filesystem::rename(file, target, error);
  if (error) {
    return false;
  }
  movedTo = "wbtrv32-originals\\" + file.filename().string();
  return true;
}

// Moves a database and its journal files to a new name.
static bool moveDatabase(const std::filesystem::path &from,
                         const std::filesystem::path &to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (error) {
    return false;
  }
  for (const char *suffix : {"-wal", "-shm"}) {
    std::filesystem::path journalFrom = from, journalTo = to;
    journalFrom += suffix;
    journalTo += suffix;
    std::filesystem::remove(journalTo, error);
    if (std::filesystem::exists(journalFrom, error)) {
      std::filesystem::rename(journalFrom, journalTo, error);
    }
  }
  return true;
}

// Gives a database an identity if it has none, and puts a marker for it in
// place of file (moving a real Btrieve file there aside first). Returns false
// only if the marker couldn't be written; the database is usable either way.
bool BtrieveDriver::takeOver(const std::filesystem::path &file,
                             const std::filesystem::path &database) {
  DatabaseIdentity identity;
  if (!sqlDatabase->readIdentity(database, identity)) {
    identity.id = newDatabaseId();
  }
  identity.source = file.filename().string();
  if (!sqlDatabase->writeIdentity(database, identity)) {
    notices.push_back("could not record its database's identity, so it was "
                      "left as an ordinary Btrieve file for now");
    return false;
  }

  std::error_code error;
  if (std::filesystem::exists(file, error) &&
      std::filesystem::file_size(file, error) > 0) {
    BtrieveDriver::Marker existing;
    if (!readMarker(file, existing)) {
      std::string movedTo;
      if (!moveOriginalAside(file, movedTo)) {
        notices.push_back("could not move the original Btrieve file aside (in "
                          "use?), so it was left in place for now");
        return false;
      }
      notices.push_back("the original Btrieve file was moved to " + movedTo +
                        " and replaced by a marker for its database");
    }
  }
  return writeMarker(file, database, identity.id);
}

// The file is a marker. Makes sure database holds the data it stands for:
// as it is, or by following a rename or copy the application made.
BtrieveError BtrieveDriver::followMarker(const std::filesystem::path &file,
                                         const BtrieveDriver::Marker &marker,
                                         const std::filesystem::path &database,
                                         bool databaseExists) {
  std::error_code error;
  DatabaseIdentity current;
  if (databaseExists && sqlDatabase->readIdentity(database, current) &&
      current.id == marker.id) {
    return BtrieveError::Success;  // the usual case
  }

  const std::filesystem::path referenced =
      file.parent_path() / marker.database;
  DatabaseIdentity original;
  const bool found = std::filesystem::exists(referenced, error) &&
                     !std::filesystem::equivalent(referenced, database, error) &&
                     sqlDatabase->readIdentity(referenced, original) &&
                     original.id == marker.id;

  if (found) {
    // Copy or rename? A copy leaves the file the data belongs to in place,
    // still a marker for the same database.
    const std::filesystem::path owner = file.parent_path() / original.source;
    BtrieveDriver::Marker ownerMarker;
    const bool copied = std::filesystem::exists(owner, error) &&
                        !std::filesystem::equivalent(owner, file, error) &&
                        readMarker(owner, ownerMarker) &&
                        ownerMarker.id == marker.id;

    if (databaseExists) {
      std::string backup;
      if (!backupDatabase(database, backup)) {
        notices.push_back("it was " +
                          std::string(copied ? "copied" : "renamed") +
                          " from " + original.source +
                          ", but its own old .db could not be moved aside (in "
                          "use?)");
        return BtrieveError::IOError;
      }
      notices.push_back("its previous data was kept as " + backup);
    }

    if (copied) {
      if (!sqlDatabase->copyDatabase(referenced, database)) {
        notices.push_back("it was copied from " + original.source +
                          ", but copying its data failed");
        return BtrieveError::IOError;
      }
      const std::string id = newDatabaseId();
      sqlDatabase->writeIdentity(database,
                                 {id, file.filename().string()});
      writeMarker(file, database, id);
      notices.push_back("it was copied from " + original.source +
                        ", so its data was copied too");
    } else {
      if (!moveDatabase(referenced, database)) {
        notices.push_back("it was renamed from " + original.source +
                          ", but its data (" + marker.database +
                          ") could not be moved (in use?)");
        return BtrieveError::IOError;
      }
      sqlDatabase->writeIdentity(database,
                                 {marker.id, file.filename().string()});
      writeMarker(file, database, marker.id);
      notices.push_back("it was renamed from " + original.source +
                        ", so its data moved with it");
    }
    return BtrieveError::Success;
  }

  if (databaseExists) {
    // The marker's database is gone but this file's own is here (put back by
    // hand, say): use it, and point the marker at it.
    notices.push_back("its marker pointed at " + marker.database +
                      ", which isn't there, so its own .db is used");
    takeOver(file, database);
    return BtrieveError::Success;
  }

  notices.push_back("its data (" + marker.database + ") is missing");
  return BtrieveError::FileNotFound;
}

BtrieveError BtrieveDriver::openDatabase(const std::filesystem::path &database,
                                         OpenMode openMode) {
  databasePath = database;
  BtrieveError error =
      sqlDatabase->open(toWideString(database).c_str(), openMode);
  if (error == BtrieveError::Success) {
    // Set Position to First Record
    performOperation(0, std::basic_string_view<uint8_t>(),
                     OperationCode::StepFirst);
  }
  return error;
}

BtrieveError BtrieveDriver::open(const wchar_t *fileName, OpenMode openMode) {
  int64_t fileModificationTimeDat = 0;
  int64_t fileModificationTimeDb = 0;

  const std::filesystem::path file(fileName);
  std::filesystem::path dbPath =
      sqlPathFor(file, sqlDatabase->getFileExtension());

  openedFilename = fileName;
  notices.clear();

  bool datExists = fileExists(fileName, fileModificationTimeDat);
  bool dbExists =
      fileExists(toWideString(dbPath).c_str(), fileModificationTimeDb);
  if (!dbExists) {
    // failed to find db, let's uppercase and try again
    std::filesystem::path dbPathUpper =
        sqlPathFor(file, toUpper(sqlDatabase->getFileExtension()));
    dbExists =
        fileExists(toWideString(dbPathUpper).c_str(), fileModificationTimeDb);
    if (dbExists) {
      dbPath = dbPathUpper;
    }
  }

  std::error_code sizeError;
  uint64_t datSize = datExists ? std::filesystem::file_size(file, sizeError) : 0;
  if (sizeError) {
    datSize = 0;
  }

  // On a case-sensitive file system the file may only exist under another
  // spelling of its extension; that still counts as present.
  bool datPresent = datExists;
  {
    const std::wstring extension = file.extension().wstring();
    for (const auto &spelling : {toUpper(extension), toLower(extension)}) {
      int64_t unused;
      std::filesystem::path variant = file;
      datPresent =
          datPresent ||
          fileExists(toWideString(variant.replace_extension(spelling)).c_str(),
                     unused);
    }
  }

  // A caller naming the database itself (NAME.db) has no separate Btrieve
  // file behind it: nothing to fingerprint, reconvert or treat as deleted.
  std::error_code sameError;
  if (dbExists && datExists &&
      std::filesystem::equivalent(file, dbPath, sameError) && !sameError) {
    return openDatabase(dbPath, openMode);
  }

  // 1. The usual case once converted: the file is a marker for its database.
  BtrieveDriver::Marker marker;
  if (datExists && datSize > 0 && readMarker(file, marker)) {
    BtrieveError error = followMarker(file, marker, dbPath, dbExists);
    if (error != BtrieveError::Success) {
      return error;
    }
    return openDatabase(dbPath, openMode);
  }

  // 2. No file at all.
  if (!datPresent) {
    if (dbExists) {
      DatabaseIdentity identity;
      SourceFingerprint recorded;
      bool tracked = sqlDatabase->readIdentity(dbPath, identity);
      if (!tracked) {
        if (sqlDatabase->open(toWideString(dbPath).c_str(), openMode) ==
            BtrieveError::Success) {
          tracked = sqlDatabase->readSourceFingerprint(recorded) &&
                    recorded.size > 0;
          sqlDatabase->close();
        }
      }
      if (tracked) {
        // Its file was deleted - the way applications reset a file. Btrieve
        // would report it missing, so do the same rather than keep serving
        // the old data.
        std::string backup;
        if (backupDatabase(dbPath, backup)) {
          notices.push_back(
              "its file was deleted, so it is treated as deleted, as Btrieve "
              "would; the old data was kept as " +
              backup);
        } else {
          notices.push_back(
              "its file was deleted, so it is treated as deleted, as Btrieve "
              "would; its .db could not be moved aside (in use?) and was left "
              "as it is");
        }
        return BtrieveError::FileNotFound;
      }
      // a database made by an older version without a file of its own
      return openDatabase(dbPath, openMode);
    }
    // nothing anywhere: let the conversion below report it missing
  }

  // 3. An empty file beside its database: the placeholder older versions
  // left. Keep the data and give it a proper marker.
  if (datExists && datSize == 0 && dbExists) {
    takeOver(file, dbPath);
    return openDatabase(dbPath, openMode);
  }

  // 4. A real Btrieve file beside an existing database.
  if (datExists && datSize > 0 && dbExists) {
    bool reconvert = false;
    DatabaseIdentity identity;
    if (sqlDatabase->readIdentity(dbPath, identity)) {
      // This database has had a marker, so a real Btrieve file here was put
      // there on purpose: a reset, restore or upgrade.
      reconvert = true;
      notices.push_back("a Btrieve file was put in place of its marker (a "
                        "reset, restore or upgrade), so it is converted");
    } else {
      // Converted by an older version, before markers: is this still the
      // file it was converted from?
      if (sqlDatabase->open(toWideString(dbPath).c_str(), OpenMode::Normal) !=
          BtrieveError::Success) {
        return BtrieveError::IOError;
      }
      SourceFingerprint recorded;
      if (sqlDatabase->readSourceFingerprint(recorded)) {
        if (datSize != recorded.size ||
            fileModificationTimeDat != recorded.modificationTime) {
          SourceFingerprint now =
              fingerprintOf(fileName, datSize, fileModificationTimeDat);
          reconvert = now.size != recorded.size || now.hash != recorded.hash;
        }
      } else {
        reconvert = fileModificationTimeDat > fileModificationTimeDb;
      }
      sqlDatabase->close();
      if (reconvert) {
        notices.push_back("its Btrieve file changed since it was converted (a "
                          "reset, restore or upgrade), so it is converted "
                          "again");
      }
    }

    if (!reconvert) {
      // same data: adopt it, replacing the Btrieve file by a marker
      takeOver(file, dbPath);
      return openDatabase(dbPath, openMode);
    }

    std::string backup;
    if (!backupDatabase(dbPath, backup)) {
      // Converting on top of a database something else has open can only
      // fail, so keep serving the data that's there and say why.
      notices.push_back(
          "but its .db could not be moved aside (in use?), so the existing "
          "data is used unchanged");
      return openDatabase(dbPath, openMode);
    }
    notices.push_back("the previous data was kept as " + backup);
    dbExists = false;
  }

  // 5. Convert the Btrieve file into a new database.
  BtrieveDatabase btrieveDatabase;
  std::unique_ptr<RecordLoader> recordLoader;
  BtrieveError error = btrieveDatabase.parseDatabase(
      fileName,
      [this, &dbPath, &btrieveDatabase, &recordLoader]() {
        recordLoader = sqlDatabase->create(toWideString(dbPath).c_str(),
                                           btrieveDatabase);
        return true;
      },
      [&recordLoader](const std::basic_string_view<uint8_t> record) {
        return recordLoader->onRecordLoaded(record);
      },
      [&recordLoader]() { recordLoader->onRecordsComplete(); });
  recordLoader.reset();

  if (error != BtrieveError::Success) {
    return error;
  }
  if (!btrieveDatabase.getConversionWarning().empty()) {
    notices.push_back("conversion problem: " +
                      btrieveDatabase.getConversionWarning());
  }
  sqlDatabase->writeSourceFingerprint(
      fingerprintOf(fileName, datSize, fileModificationTimeDat));
  sqlDatabase->close();

  sqlDatabase->writeIdentity(dbPath, {newDatabaseId(), file.filename().string()});
  takeOver(file, dbPath);
  return openDatabase(dbPath, openMode);
}

void BtrieveDriver::close() {
  sqlDatabase->close();
  // release ownership and delete
  sqlDatabase.reset(nullptr);
}

BtrieveError BtrieveDriver::performOperation(
    int keyNumber, std::basic_string_view<uint8_t> keyData,
    OperationCode operationCode) {
  switch (operationCode) {
    case OperationCode::Delete:
      return sqlDatabase->deleteRecord();
      /* lock biases, which we don't support / care about
      +100 Single wait record lock.
      +200 Single no-wait record lock.
      +300 Multiple wait record lock.
      +400 Multiple no-wait record lock.
      */
    case OperationCode::StepFirst:
    case OperationCode::StepFirst + 100:
    case OperationCode::StepFirst + 200:
    case OperationCode::StepFirst + 300:
    case OperationCode::StepFirst + 400:
      return sqlDatabase->stepFirst();
    case OperationCode::StepLast:
    case OperationCode::StepLast + 100:
    case OperationCode::StepLast + 200:
    case OperationCode::StepLast + 300:
    case OperationCode::StepLast + 400:
      return sqlDatabase->stepLast();
    case OperationCode::StepNext:
    case OperationCode::StepNext + 100:
    case OperationCode::StepNext + 200:
    case OperationCode::StepNext + 300:
    case OperationCode::StepNext + 400:
    case OperationCode::StepNextExtended:
    case OperationCode::StepNextExtended + 100:
    case OperationCode::StepNextExtended + 200:
    case OperationCode::StepNextExtended + 300:
    case OperationCode::StepNextExtended + 400:
      return sqlDatabase->stepNext();
    case OperationCode::StepPrevious:
    case OperationCode::StepPrevious + 100:
    case OperationCode::StepPrevious + 200:
    case OperationCode::StepPrevious + 300:
    case OperationCode::StepPrevious + 400:
    case OperationCode::StepPreviousExtended:
    case OperationCode::StepPreviousExtended + 100:
    case OperationCode::StepPreviousExtended + 200:
    case OperationCode::StepPreviousExtended + 300:
    case OperationCode::StepPreviousExtended + 400:
      return sqlDatabase->stepPrevious();
    default:
        // fall through
        ;
  }

  if (usesPreviousQuery(operationCode)) {
    if (!previousQuery) {
      return BtrieveError::InvalidPositioning;
    } else if (previousQuery->getKey()->getNumber() != keyNumber) {
      return BtrieveError::DifferentKeyNumber;
    }
  } else {
    // this is a new query
    if (keyNumber < 0 ||
        static_cast<unsigned int>(keyNumber) >= sqlDatabase->getKeys().size()) {
      return BtrieveError::InvalidKeyNumber;
    }

    const Key *key = &(sqlDatabase->getKeys()[keyNumber]);

    // possibly truncate the length of keyData, since some clients just specify
    // the max of 255 when the key is much smaller
    int exceededSize = keyData.size() - key->getLength();
    if (exceededSize > 0) {
      keyData.remove_suffix(exceededSize);
    }

    previousQuery = std::move(
        sqlDatabase->newQuery(sqlDatabase->getPosition(), key, keyData));
  }

  // always using previousQuery from this point onward
  BtrieveError error;
  switch (operationCode) {
    // these operations continue from a set logical position
    case OperationCode::AcquireNext:
    case OperationCode::QueryNext:
      return sqlDatabase->getByKeyNext(previousQuery.get());
    case OperationCode::AcquirePrevious:
    case OperationCode::QueryPrevious:
      return sqlDatabase->getByKeyPrevious(previousQuery.get());
    // the following operations set logical position
    case OperationCode::AcquireFirst:
    case OperationCode::QueryFirst:
      error = sqlDatabase->getByKeyFirst(previousQuery.get());
      break;
    case OperationCode::AcquireLast:
    case OperationCode::QueryLast:
      error = sqlDatabase->getByKeyLast(previousQuery.get());
      break;
    case OperationCode::AcquireEqual:
    case OperationCode::QueryEqual:
      error = sqlDatabase->getByKeyEqual(previousQuery.get());
      break;
    case OperationCode::AcquireGreater:
    case OperationCode::QueryGreater:
      error = sqlDatabase->getByKeyGreater(previousQuery.get());
      break;
    case OperationCode::AcquireGreaterOrEqual:
    case OperationCode::QueryGreaterOrEqual:
      error = sqlDatabase->getByKeyGreaterOrEqual(previousQuery.get());
      break;
    case OperationCode::AcquireLess:
    case OperationCode::QueryLess:
      error = sqlDatabase->getByKeyLess(previousQuery.get());
      break;
    case OperationCode::AcquireLessOrEqual:
    case OperationCode::QueryLessOrEqual:
      error = sqlDatabase->getByKeyLessOrEqual(previousQuery.get());
      break;
    default:
      return BtrieveError::OperationNotAllowed;
  }

  // if we had an error, clear out previousQuery as if it were never set above
  if (error != BtrieveError::Success) {
    previousQuery.reset(nullptr);
  }

  return error;
}

}  // namespace btrieve

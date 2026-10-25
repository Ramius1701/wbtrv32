#ifndef __BTRIEVE_DRIVER_H_
#define __BTRIEVE_DRIVER_H_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ErrorCode.h"
#include "OpenMode.h"
#include "OperationCode.h"
#include "Record.h"
#include "SqlDatabase.h"
#include "Text.h"

namespace btrieve {
// Where a Btrieve file's data lives once converted. NAME.DAT keeps the NAME.db
// it always had (and NAME.db names itself); any other extension gets
// NAME.EXT.db, so files that differ only in extension (a .MST master copy
// beside its .DAT, say) never share one database.
std::filesystem::path sqlPathFor(const std::filesystem::path &btrieveFile,
                                 const std::wstring &sqlExtension);

// Moves a .db (and its -wal/-shm, which may hold committed data) aside as
// .db.bak instead of deleting it, replacing any previous backup. Returns false
// if the .db couldn't be moved (e.g. it's open elsewhere), leaving it as it
// was. backupName receives the backup's file name.
bool backupDatabase(const std::filesystem::path &dbPath,
                    std::string &backupName);

// Implements the Btrieve Driver functionality, e.g. querying and iterating
// through records.
class BtrieveDriver {
 public:
  // With trackSourceFile_, a converted Btrieve file is replaced by a marker
  // for its database (see markerMagic()). Turn it off when something else
  // keeps writing the .DAT, as stock Btrieve does under the parity shim.
  BtrieveDriver(std::unique_ptr<SqlDatabase> sqlDatabase_,
                bool trackSourceFile_ = true)
      : sqlDatabase(std::move(sqlDatabase_)),
        trackSourceFile(trackSourceFile_) {}

  BtrieveDriver(BtrieveDriver &&driver)
      : sqlDatabase(std::move(driver.sqlDatabase)),
        trackSourceFile(driver.trackSourceFile) {}

  ~BtrieveDriver();

  BtrieveError open(const wchar_t *fileName,
                    OpenMode openMode = OpenMode::Normal);

  // Closes an opened database.
  void close();

  unsigned int getRecordLength() const {
    return sqlDatabase->getRecordLength();
  }

  unsigned int getRecordCount() const { return sqlDatabase->getRecordCount(); }

  unsigned int getUniqueKeyCount(const Key &key) const {
    return sqlDatabase->getUniqueKeyCount(key);
  }

  bool isVariableLengthRecords() const {
    return sqlDatabase->isVariableLengthRecords();
  }

  const std::vector<Key> &getKeys() const { return sqlDatabase->getKeys(); }

  unsigned int getPosition() const { return sqlDatabase->getPosition(); }

  void setPosition(unsigned int position) {
    return sqlDatabase->setPosition(position);
  }

  bool hasCurrentRecord() const { return sqlDatabase->hasCurrentRecord(); }

  std::pair<bool, Record> getRecord() { return getRecord(getPosition()); }

  std::pair<bool, Record> getRecord(unsigned int position) {
    return sqlDatabase->getRecord(position);
  }

  bool deleteAll() { return sqlDatabase->deleteAll(); }

  std::pair<BtrieveError, unsigned int> insertRecord(
      std::basic_string_view<uint8_t> record) {
    return sqlDatabase->insertRecord(record);
  }

  BtrieveError updateRecord(unsigned int id,
                            std::basic_string_view<uint8_t> record) {
    return sqlDatabase->updateRecord(id, record);
  }

  BtrieveError performOperation(int keyNumber,
                                std::basic_string_view<uint8_t> key,
                                OperationCode operationCode);

  BtrieveError logicalCurrencySeek(int keyNumber, unsigned int position) {
    BtrieveError ret;

    previousQuery = sqlDatabase->logicalCurrencySeek(keyNumber, position, ret);

    return ret;
  }

  // Forgets the logical position, so Get Next/Previous fail until a Get
  // establishes a new one.
  void clearLogicalCurrency() { previousQuery.reset(); }

  std::basic_string<wchar_t> getOpenedFilename() { return openedFilename; }

  std::optional<OwnerInfo> getOwner() { return sqlDatabase->getOwner(); }

  void setOwner(const std::optional<OwnerInfo> &owner) {
    sqlDatabase->setOwner(owner);
  }

  bool beginUserTransaction() { return sqlDatabase->beginUserTransaction(); }
  bool endUserTransaction() { return sqlDatabase->endUserTransaction(); }
  bool abortUserTransaction() {
    previousQuery.reset();
    return sqlDatabase->abortUserTransaction();
  }

  // Once a Btrieve file's data is in its database, the file itself is replaced
  // by a small marker naming that database and its id (the original is kept in
  // a wbtrv32-originals folder beside it). Applications that rename, copy or
  // delete the file then rename, copy or delete exactly that data, and a real
  // Btrieve file put back in its place is recognised as new data.
  static const char *markerMagic() { return "WBTRV32 DATA FILE"; }
  struct Marker {
    std::string database;  // file name of the database, beside the marker
    std::string id;
  };
  static std::string newDatabaseId();
  static bool writeMarker(const std::filesystem::path &markerFile,
                          const std::filesystem::path &database,
                          const std::string &id);

  // Things worth telling the sysop about from the last open(): a conversion
  // that didn't add up, a .DAT that changed or vanished since it was
  // converted, and where the previous .db was kept.
  const std::vector<std::string> &getNotices() const { return notices; }

  // Btrieve keeps a position, current record and key path per open handle,
  // not per file. Handles on the same file share one driver, so each handle
  // owns a Cursor and activates it before every operation: the driver saves
  // the previous handle's state into its Cursor and puts this one's in place.
  struct Cursor {
    unsigned int position = 0;
    bool currentRecord = false;
    std::unique_ptr<Query> query;
  };

  void activate(const std::shared_ptr<Cursor> &cursor) {
    // a closed handle's Cursor is gone, so its state is simply dropped
    std::shared_ptr<Cursor> active = activeCursor.lock();
    if (active == cursor) {
      return;
    }
    if (active != nullptr) {
      active->position = sqlDatabase->getPosition();
      active->currentRecord = sqlDatabase->hasCurrentRecord();
      active->query = std::move(previousQuery);
    }
    sqlDatabase->setPosition(cursor != nullptr ? cursor->position : 0);
    if (cursor == nullptr || !cursor->currentRecord) {
      sqlDatabase->clearCurrentRecord();
    }
    previousQuery =
        cursor != nullptr ? std::move(cursor->query) : std::unique_ptr<Query>();
    activeCursor = cursor;
  }

 private:
  BtrieveError openDatabase(const std::filesystem::path &database,
                            OpenMode openMode);
  bool takeOver(const std::filesystem::path &file,
                const std::filesystem::path &database);
  BtrieveError followMarker(const std::filesystem::path &file,
                            const Marker &marker,
                            const std::filesystem::path &database,
                            bool databaseExists);

  std::unique_ptr<SqlDatabase> sqlDatabase;
  bool trackSourceFile;
  std::unique_ptr<Query> previousQuery;
  std::basic_string<wchar_t> openedFilename;
  std::vector<std::string> notices;
  std::filesystem::path databasePath;  // set when the database is opened
  // Not owned: each handle owns its Cursor, which may be closed at any time.
  std::weak_ptr<Cursor> activeCursor;
};
}  // namespace btrieve
#endif

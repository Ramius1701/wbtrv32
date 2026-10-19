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
// Implements the Btrieve Driver functionality, e.g. querying and iterating
// through records.
class BtrieveDriver {
 public:
  BtrieveDriver(SqlDatabase *sqlDatabase_) : sqlDatabase(sqlDatabase_) {}

  BtrieveDriver(BtrieveDriver &&driver)
      : sqlDatabase(std::move(driver.sqlDatabase)) {}

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

  // Btrieve keeps a position (and key path) per open handle, not per file.
  // Handles opened on the same file share one driver, so each handle owns a
  // Cursor and activates it before every operation; the driver swaps the
  // previous handle's state out and this one's in.
  struct Cursor {
    unsigned int position = 0;
    bool currentRecord = false;
    std::unique_ptr<Query> query;
  };

  void activate(Cursor *cursor) {
    if (activeCursor == cursor) {
      return;
    }
    if (activeCursor != nullptr) {
      activeCursor->position = sqlDatabase->getPosition();
      activeCursor->currentRecord = sqlDatabase->hasCurrentRecord();
      activeCursor->query = std::move(previousQuery);
    }
    sqlDatabase->setPosition(cursor != nullptr ? cursor->position : 0);
    if (cursor == nullptr || !cursor->currentRecord) {
      sqlDatabase->clearCurrentRecord();
    }
    previousQuery =
        cursor != nullptr ? std::move(cursor->query) : std::unique_ptr<Query>();
    activeCursor = cursor;
  }

  bool readOwner(OwnerInfo &owner) {
    return sqlDatabase->readOwner(databasePath, owner);
  }
  bool writeOwner(const OwnerInfo *owner) {
    return sqlDatabase->writeOwner(databasePath, owner);
  }

  bool beginUserTransaction() { return sqlDatabase->beginUserTransaction(); }
  bool endUserTransaction() { return sqlDatabase->endUserTransaction(); }
  bool abortUserTransaction() { return sqlDatabase->abortUserTransaction(); }

  // Called before a handle's Cursor is destroyed.
  void release(Cursor *cursor) {
    if (activeCursor == cursor) {
      activeCursor = nullptr;
    }
  }

  // Where a Btrieve file's data lives once converted. NAME.DAT keeps the
  // NAME.db it always had (and NAME.db names itself); any other extension gets
  // NAME.EXT.db, so files that differ only in extension (a .MST master copy
  // beside its .DAT, say) never share one database.
  static std::filesystem::path sqlPathFor(
      const std::filesystem::path &btrieveFile,
      const std::wstring &sqlExtension);

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

 private:
  std::unique_ptr<SqlDatabase> sqlDatabase;
  std::unique_ptr<Query> previousQuery;
  std::basic_string<wchar_t> openedFilename;
  std::vector<std::string> notices;
  Cursor *activeCursor = nullptr;
  std::filesystem::path databasePath;  // set when the database is opened

  BtrieveError openDatabase(const std::filesystem::path &database,
                            OpenMode openMode);
  bool takeOver(const std::filesystem::path &file,
                const std::filesystem::path &database);
  BtrieveError followMarker(const std::filesystem::path &file,
                            const Marker &marker,
                            const std::filesystem::path &database,
                            bool databaseExists);
};
}  // namespace btrieve
#endif

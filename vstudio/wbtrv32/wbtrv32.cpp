// wbtrv32.cpp : Defines the exported functions for the DLL.
//
#include "wbtrv32.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "btrieve/BtrieveDriver.h"
#include "btrieve/ErrorCode.h"
#include "btrieve/OperationCode.h"
#include "btrieve/SqliteDatabase.h"
#include "btrieve/Text.h"
#include "framework.h"

// Define this to pop up a messagebox when wbtrv32.dll is loaded, to allow you
// to attach a debugger on startup
//
// #define DEBUG_ATTACH

// Define this to log errors to wbtrv32.log
#define LOG_TO_FILE

// Written to the log at startup so every log says which build produced it.
#define WBTRV32_VERSION "1.0.4+local"

// How much goes to wbtrv32.log. Set with LogLevel= in the [wbtrv32] section of
// wbtrv32.ini in the current directory, or the WBTRV32_LOGLEVEL environment
// variable (which wins).
enum LogLevel : int {
  LogOff = 0,
  // failures and conversion notices, but not the statuses applications get as
  // routine answers (key not found, end of file, partial read)
  LogProblems = 1,
  LogAllStatuses = 2,  // every non-zero status
  LogEverything = 3,   // also every file open
};

static int _logLevel = LogProblems;

#ifdef DEBUG_ATTACH
#include "Psapi.h"
#endif

#ifdef WIN32
#include <share.h>
#endif

using namespace btrieve;

// One entry per open handle (position block). Handles on the same file share
// the driver, and each has its own Cursor so their positions stay separate.
struct OpenHandle {
  std::shared_ptr<BtrieveDriver> driver;
  std::unique_ptr<BtrieveDriver::Cursor> cursor;
  // the mode this handle was opened in; the shared database itself is always
  // opened writable, and read-only / exclusive are enforced per handle
  OpenMode mode = OpenMode::Normal;
  // false if the file has an owner and this handle was opened without the
  // owner name (only possible where the owner allows read-only access)
  bool ownerRights = true;

  OpenHandle(std::shared_ptr<BtrieveDriver> driver_, OpenMode mode_)
      : driver(std::move(driver_)),
        cursor(std::make_unique<BtrieveDriver::Cursor>()),
        mode(mode_) {}
  OpenHandle(OpenHandle &&) = default;
  ~OpenHandle() {
    if (driver && cursor) {
      driver->release(cursor.get());
    }
  }
};

static std::unordered_map<std::basic_string<wchar_t>, OpenHandle> _openFiles;

static const OpenHandle *findHandle(LPVOID lpPositionBlock);

#ifdef LOG_TO_FILE
// A plain pointer, closed only by processDetach(): a static object with a
// destructor would be destroyed at exit before the Linux .so destructor below
// runs processDetach(), which would then close the log a second time.
static FILE *_logFile = nullptr;
#endif

static int readLogLevel() {
#ifdef WIN32
  char fromEnvironment[16];
  size_t found = 0;
  if (getenv_s(&found, fromEnvironment, sizeof(fromEnvironment),
               "WBTRV32_LOGLEVEL") == 0 &&
      found > 1) {
    return std::clamp(atoi(fromEnvironment), int{LogOff}, int{LogEverything});
  }
#else
  const char *fromEnvironment = getenv("WBTRV32_LOGLEVEL");
  if (fromEnvironment != nullptr && *fromEnvironment != 0) {
    return std::clamp(atoi(fromEnvironment), int{LogOff}, int{LogEverything});
  }
#endif
#ifdef WIN32
  // GetPrivateProfileInt looks in the Windows directory unless given a path
  wchar_t iniPath[MAX_PATH];
  if (GetFullPathNameW(L"wbtrv32.ini", ARRAYSIZE(iniPath), iniPath,
                       nullptr) != 0) {
    return std::clamp(static_cast<int>(GetPrivateProfileIntW(
                          L"wbtrv32", L"LogLevel", LogProblems, iniPath)),
                      int{LogOff}, int{LogEverything});
  }
#endif
  return LogProblems;
}

#ifdef LOG_TO_FILE
static void writeLogTimestamp() {
  if (!_logFile) {
    return;
  }
  time_t now = time(nullptr);
  struct tm local;
#ifdef WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  size_t len = strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", &local);
  fwrite(stamp, 1, len, _logFile);
}

static void writeVersionStamp() {
  if (!_logFile) {
    return;
  }
  char program[MAX_PATH] = "?";
  char library[MAX_PATH] = "?";
#ifdef WIN32
  GetModuleFileNameA(nullptr, program, ARRAYSIZE(program));
  HMODULE self = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&readLogLevel), &self)) {
    GetModuleFileNameA(self, library, ARRAYSIZE(library));
  }
#endif
  writeLogTimestamp();
  fprintf(_logFile,
          "wbtrv32 %s (built %s %s) %s loaded by %s, log level %d\r\n",
          WBTRV32_VERSION, __DATE__, __TIME__, library, program, _logLevel);
  fflush(_logFile);
}
#endif  // LOG_TO_FILE

#ifndef WIN32
// DllMain (dllmain.cpp) is a Windows-only entry point and is never invoked
// when this is built as a Linux .so/macOS .dylib, so processAttach()/
// processDetach() otherwise never run outside of Windows.
__attribute__((constructor)) static void wbtrv32_soConstructor() {
  wbtrv32::processAttach();
}

__attribute__((destructor)) static void wbtrv32_soDestructor() {
  wbtrv32::processDetach();
}
#endif  // !WIN32

void wbtrv32::processAttach() {
#ifdef DEBUG_ATTACH
  {
    wchar_t buf[MAX_PATH];

    GetProcessImageFileName(GetCurrentProcess(), buf, ARRAYSIZE(buf));
    MessageBox(NULL, TEXT("Attach now"), buf, MB_OK);
  }
#endif

  _logLevel = readLogLevel();

#ifdef LOG_TO_FILE
  if (_logLevel == LogOff) {
    return;
  }
#ifdef WIN32
  _logFile = _wfsopen(L"wbtrv32.log", L"ab", _SH_DENYWR);
#else
  _logFile = fopen("/tmp/wbtrv32.log", "ab");
#endif  // WIN32
  writeVersionStamp();
#endif  // LOG_TO_FILE
}

void wbtrv32::processDetach() {
#ifdef LOG_TO_FILE
  if (_logFile) {
    fclose(_logFile);
    _logFile = nullptr;
  }
#endif
}

static BtrieveDriver *getOpenDatabase(LPVOID lpPositioningBlock) {
  wchar_t guidStr[64];

  if (lpPositioningBlock == nullptr) {
    return nullptr;
  }

  StringFromGUID2(*reinterpret_cast<GUID *>(lpPositioningBlock), guidStr,
                  ARRAYSIZE(guidStr));

  auto iterator = _openFiles.find(std::basic_string<wchar_t>(guidStr));
  if (iterator == _openFiles.end()) {
    return nullptr;
  }
  // every operation goes through here, so this is where the handle's own
  // position is put in place
  iterator->second.driver->activate(iterator->second.cursor.get());
  return iterator->second.driver.get();
}

struct BtrieveCommand {
  OperationCode operation;

  LPVOID lpPositionBlock;

  LPVOID lpDataBuffer;
  LPDWORD lpdwDataBufferLength;

  LPVOID lpKeyBuffer;
  uint8_t lpKeyBufferLength;

  int8_t keyNumber;
};

static void debug(const BtrieveCommand &command, const char *format, ...) {
  BtrieveDriver *driver = getOpenDatabase(command.lpPositionBlock);
  char buf[512];
  int len;

#ifdef LOG_TO_FILE
  writeLogTimestamp();
#endif

  if (driver != nullptr) {
    len = snprintf(
        buf, sizeof(buf), "[%s]: ",
        btrieve::toStdString(driver->getOpenedFilename().c_str()).c_str());

#ifdef WIN32
    OutputDebugStringA(buf);
#endif

#ifdef LOG_TO_FILE
    if (_logFile) {
      fwrite(buf, 1, len, _logFile);
    }
#endif
  }

  va_list args;
  va_start(args, format);
  len = vsnprintf(buf, sizeof(buf) - 2, format, args);
  va_end(args);

  // vsnprintf returns the length it wanted, not what fit
  if (len < 0) {
    len = 0;
  } else if (len > static_cast<int>(sizeof(buf)) - 3) {
    len = static_cast<int>(sizeof(buf)) - 3;
  }

  // append cr/lf
  buf[len] = '\r';
  buf[len + 1] = '\n';
  buf[len + 2] = 0;
#ifdef WIN32
  OutputDebugStringA(buf);
#endif

#ifdef LOG_TO_FILE
  if (_logFile) {
    fwrite(buf, 1, len + 2, _logFile);
    fflush(_logFile);
  }
#endif
}

// Registers a new handle for driver and makes it the active one. A brand-new
// driver's state (positioned on the first record by open()) becomes the
// handle's; a handle added to a driver that's already open starts on the
// first record too, without disturbing the other handles.
// The directory set with Set Directory (17); empty = the process's own.
static std::wstring _btrieveDirectory;

// An application transaction (Begin Transaction, 19) covers every file the
// application has open or opens before it ends. Each file's database joins
// once; the list keeps them alive until End/Abort even if a file is closed in
// between, so its changes still commit or roll back with the rest.
static bool _transactionActive = false;
static std::vector<std::shared_ptr<BtrieveDriver>> _transactionDrivers;

static void joinTransaction(const std::shared_ptr<BtrieveDriver> &driver) {
  if (!_transactionActive) {
    return;
  }
  for (const auto &member : _transactionDrivers) {
    if (member == driver) {
      return;
    }
  }
  driver->beginUserTransaction();
  _transactionDrivers.push_back(driver);
}

// Full path of a file name as an application gave it: relative names are
// taken from the Set Directory directory when one was set.
static void resolveFileName(const char *name, wchar_t *fullPath,
                            DWORD fullPathLength) {
  std::filesystem::path path(btrieve::toWideString(name));
  if (!_btrieveDirectory.empty() && path.is_relative()) {
    path = std::filesystem::path(_btrieveDirectory) / path;
  }
  GetFullPathNameW(path.wstring().c_str(), fullPathLength, fullPath, nullptr);
}

// The owner name an Open supplied in its data buffer, if any.
static std::string suppliedOwnerName(const BtrieveCommand &command) {
  if (command.lpDataBuffer == nullptr ||
      command.lpdwDataBufferLength == nullptr ||
      *command.lpdwDataBufferLength == 0) {
    return std::string();
  }
  const char *name = reinterpret_cast<const char *>(command.lpDataBuffer);
  return std::string(name, strnlen(name, *command.lpdwDataBufferLength));
}

// Checks an Open against the file's owner. Returns InvalidOwner (51) when the
// owner name is required and wasn't given; otherwise the open goes ahead,
// read-only when the owner allows only that without the name.
static BtrieveError checkOwner(const BtrieveCommand &command,
                               BtrieveDriver &driver, OpenMode &mode,
                               bool &ownerRights) {
  OwnerInfo owner;
  ownerRights = true;
  if (!driver.readOwner(owner) || owner.name.empty() ||
      suppliedOwnerName(command) == owner.name) {
    return BtrieveError::Success;
  }
  if (owner.mode == 1 || owner.mode == 3) {
    mode = OpenMode::ReadOnly;
    ownerRights = false;
    return BtrieveError::Success;
  }
  return BtrieveError::InvalidOwner;
}

static void AddToOpenFiles(BtrieveCommand &command,
                           std::shared_ptr<BtrieveDriver> driver,
                           bool newDriver, OpenMode mode,
                           bool ownerRights = true) {
  // if this position block already references an open database (e.g. the
  // caller re-opened without closing first), close that one out first so we
  // don't orphan it in _openFiles -- overwriting the GUID below without doing
  // this would leak the previous entry (and its underlying sqlite
  // connection) for the lifetime of the process.
  if (command.lpPositionBlock != nullptr) {
    wchar_t existingGuidStr[64];
    StringFromGUID2(*reinterpret_cast<GUID *>(command.lpPositionBlock),
                    existingGuidStr, ARRAYSIZE(existingGuidStr));
    _openFiles.erase(existingGuidStr);
  }

  // add to my list of open files
  GUID guid;
  CoCreateGuid(&guid);

  wchar_t guidStr[64];
  StringFromGUID2(guid, guidStr, ARRAYSIZE(guidStr));

  OpenHandle handle(driver, mode);
  handle.ownerRights = ownerRights;
  if (newDriver) {
    // adopt the position open() set up instead of resetting it
    handle.cursor->position = driver->getPosition();
  }
  BtrieveDriver::Cursor *cursor = handle.cursor.get();
  _openFiles.emplace(std::basic_string<wchar_t>(guidStr), std::move(handle));
  driver->activate(cursor);
  if (!newDriver) {
    driver->performOperation(0, std::basic_string_view<uint8_t>(),
                             OperationCode::StepFirst);
  }

  // write the GUID in the pos block for other calls
  memset(command.lpPositionBlock, 0, POSBLOCK_LENGTH);
  memcpy(command.lpPositionBlock, &guid, sizeof(GUID));
}

static BtrieveError Open(BtrieveCommand &command) {
  wchar_t fullPathFileName[MAX_PATH];

  const char *lpszFilename =
      reinterpret_cast<const char *>(command.lpKeyBuffer);
  auto openMode = static_cast<OpenMode>(command.keyNumber);

  if (_logLevel >= LogEverything) {
    debug(command, "Attempting to open %s with openMode %d", lpszFilename,
          openMode);
  }

  resolveFileName(lpszFilename, fullPathFileName, ARRAYSIZE(fullPathFileName));
  // One spelling per file: expand 8.3 short names (DIALGL~1.DAT) to the long
  // name, so every way of naming a file reaches the same open driver rather
  // than a second one on the same database.
  {
    wchar_t longPathFileName[MAX_PATH];
    const DWORD length = GetLongPathNameW(fullPathFileName, longPathFileName,
                                          ARRAYSIZE(longPathFileName));
    if (length > 0 && length < ARRAYSIZE(longPathFileName)) {
      wcscpy_s(fullPathFileName, longPathFileName);
    }
  }

  // see if we've already opened this file
  std::shared_ptr<BtrieveDriver> shared;
  for (auto iterator = _openFiles.begin(); iterator != _openFiles.end();
       ++iterator) {
    if (!_wcsicmp(iterator->second.driver->getOpenedFilename().c_str(),
                  fullPathFileName)) {
      // exclusive means exclusive: no second open while one handle has it,
      // and no exclusive open while another handle has the file open
      if (openMode == OpenMode::ExclusiveAccess ||
          iterator->second.mode == OpenMode::ExclusiveAccess) {
        return BtrieveError::IncompatibleMode;
      }
      shared = iterator->second.driver;
    }
  }
  if (!shared) {
    for (const auto &member : _transactionDrivers) {
      if (!_wcsicmp(member->getOpenedFilename().c_str(), fullPathFileName)) {
        shared = member;  // closed inside the transaction: same connection
      }
    }
  }
  if (shared) {
    // already got one? share its database, with a position of its own
    bool ownerRights;
    BtrieveError ownerError =
        checkOwner(command, *shared, openMode, ownerRights);
    if (ownerError != BtrieveError::Success) {
      return ownerError;
    }
    AddToOpenFiles(command, shared, false, openMode, ownerRights);
    return BtrieveError::Success;
  }

  std::shared_ptr<BtrieveDriver> driver =
      std::make_shared<BtrieveDriver>(new SqliteDatabase());

  // conversion problems, reconversions and backups are always worth a line,
  // whether or not the open then succeeds
  auto logNotices = [&]() {
    if (_logLevel >= LogProblems) {
      for (const auto &notice : driver->getNotices()) {
        debug(command, "%s: %s", lpszFilename, notice.c_str());
      }
    }
  };

  BtrieveError error;
  try {
    // The database is shared by every handle on the file, so it's opened
    // writable whatever this handle asked for; read-only is enforced per
    // handle (writes through it return 46).
    error = driver->open(fullPathFileName, openMode == OpenMode::ReadOnly
                                               ? OpenMode::Normal
                                               : openMode);
  } catch (...) {
    logNotices();
    throw;
  }
  logNotices();
  if (error != BtrieveError::Success) {
    return error;
  }

  bool ownerRights;
  BtrieveError ownerError = checkOwner(command, *driver, openMode, ownerRights);
  if (ownerError != BtrieveError::Success) {
    return ownerError;
  }
  joinTransaction(driver);
  AddToOpenFiles(command, driver, true, openMode, ownerRights);

  return BtrieveError::Success;
}

static BtrieveError Close(BtrieveCommand &command) {
  wchar_t guidStr[64];
  StringFromGUID2(*reinterpret_cast<GUID *>(command.lpPositionBlock), guidStr,
                  ARRAYSIZE(guidStr));

  if (_openFiles.find(guidStr) == _openFiles.end()) {
    return BtrieveError::FileNotOpen;
  }

  _openFiles.erase(guidStr);
  memset(command.lpPositionBlock, 0, POSBLOCK_LENGTH);
  return BtrieveError::Success;
}

// The index of segment's ACS in acsDefinitions, or acsDefinitions.size() if
// it is not there.
static size_t acsNumber(const std::vector<const KeyDefinition *> &acsDefinitions,
                        const KeyDefinition &segment) {
  for (size_t i = 0; i < acsDefinitions.size(); ++i) {
    if (!strcmp(acsDefinitions[i]->getACSName(), segment.getACSName()) &&
        !memcmp(acsDefinitions[i]->getACS(), segment.getACS(), ACS_LENGTH)) {
      return i;
    }
  }
  return acsDefinitions.size();
}

static BtrieveError Stat(BtrieveCommand &command) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  const bool includeFileVersion = (command.keyNumber == -1);

  if (command.lpKeyBuffer && command.lpKeyBufferLength > 0) {
    // in a sane world I would zero all this memory out, but wgserver.exe
    // crashes
    *reinterpret_cast<uint8_t *>(command.lpKeyBuffer) = 0;
  }

  size_t totalKeysIncludingSegmentedKeys = 0;
  // the distinct ACS tables, which follow the key specs, numbered in the
  // order the keys first use them
  std::vector<const KeyDefinition *> acsDefinitions;
  for (const auto &key : btrieveDriver->getKeys()) {
    totalKeysIncludingSegmentedKeys += key.getSegments().size();
    for (const auto &segment : key.getSegments()) {
      if (segment.requiresACS() &&
          acsNumber(acsDefinitions, segment) == acsDefinitions.size()) {
        acsDefinitions.push_back(&segment);
      }
    }
  }

  const unsigned int requiredSize = static_cast<unsigned int>(
      sizeof(wbtrv32::FILESPEC) +
      (totalKeysIncludingSegmentedKeys * sizeof(wbtrv32::KEYSPEC)) +
      (acsDefinitions.size() * sizeof(wbtrv32::ACSCREATEDATA)));
  if (*command.lpdwDataBufferLength < requiredSize) {
    return BtrieveError::DataBufferLengthOverrun;
  }

  // in a sane world I would zero all the memory out in command.lpDataBuffer
  // that was passed to me, but wgserver.exe crashes

  *command.lpdwDataBufferLength = requiredSize;

  const wbtrv32::LPFILESPEC lpFileSpec =
      reinterpret_cast<wbtrv32::LPFILESPEC>(command.lpDataBuffer);
  lpFileSpec->logicalFixedRecordLength = btrieveDriver->getRecordLength();
  lpFileSpec->pageSize = 4096;  // doesn't matter, not needed for sqlite
  lpFileSpec->numberOfKeys = static_cast<uint8_t>(
      btrieveDriver->getKeys()
          .size());  // note: this is not totalKeysIncludingSegmentedKeys
  lpFileSpec->fileVersion =
      includeFileVersion ? 0x60 : 0;  // emulate btrieve 6.0

  lpFileSpec->recordCount = btrieveDriver->getRecordCount();
  lpFileSpec->fileFlags = btrieveDriver->isVariableLengthRecords() ? 1 : 0;
  lpFileSpec->numExtraPointers = 0;
  lpFileSpec->physicalPageSize =
      0;  // only set for compressed, which we don't do
  lpFileSpec->preallocatedPages = 0;

  wbtrv32::LPKEYSPEC lpKeySpec =
      reinterpret_cast<wbtrv32::LPKEYSPEC>(lpFileSpec + 1);
  uint8_t keyNumber = 0;
  for (const auto &key : btrieveDriver->getKeys()) {
    const unsigned int uniqueKeys = btrieveDriver->getUniqueKeyCount(key);
    for (const auto &segment : key.getSegments()) {
      lpKeySpec->position = segment.getPosition();
      lpKeySpec->length = segment.getLength();
      lpKeySpec->attributes = segment.getAttributes();
      lpKeySpec->uniqueKeys = uniqueKeys;
      lpKeySpec->extendedDataType = segment.getDataType();
      lpKeySpec->nullValue = segment.getNullValue();
      lpKeySpec->reserved = 0;
      lpKeySpec->number = keyNumber;
      lpKeySpec->acsNumber =
          segment.requiresACS()
              ? static_cast<uint8_t>(acsNumber(acsDefinitions, segment))
              : 0;
      ++lpKeySpec;
    }

    ++keyNumber;
  }

  wbtrv32::LPACSCREATEDATA lpAcsData =
      reinterpret_cast<wbtrv32::LPACSCREATEDATA>(lpKeySpec);
  for (const KeyDefinition *definition : acsDefinitions) {
    lpAcsData->header = 0xAC;
    memset(lpAcsData->name, 0, sizeof(lpAcsData->name));
    memcpy(lpAcsData->name, definition->getACSName(),
           std::min(strlen(definition->getACSName()), sizeof(lpAcsData->name)));
    memcpy(lpAcsData->acs, definition->getACS(), ACS_LENGTH);
    ++lpAcsData;
  }

  return BtrieveError::Success;
}

static BtrieveError Delete(BtrieveCommand &command) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  return btrieveDriver->performOperation(-1, std::basic_string_view<uint8_t>(),
                                         OperationCode::Delete);
}

static BtrieveError Step(BtrieveCommand &command) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  auto oldPosition = btrieveDriver->getPosition();
  auto result = btrieveDriver->performOperation(
      -1, std::basic_string_view<uint8_t>(), command.operation);
  if (result != BtrieveError::Success) {
    return result;
  }

  auto record = btrieveDriver->getRecord();
  if (!record.first) {
    btrieveDriver->setPosition(oldPosition);
    return BtrieveError::IOError;
  }

  // Btrieve keeps the record current even when it does not fit
  if (*command.lpdwDataBufferLength < record.second.getData().size()) {
    return BtrieveError::DataBufferLengthOverrun;
  }

  *command.lpdwDataBufferLength =
      static_cast<DWORD>(record.second.getData().size());
  memcpy(command.lpDataBuffer, record.second.getData().data(),
         record.second.getData().size());

  return BtrieveError::Success;
}

static BtrieveError GetPosition(BtrieveCommand &command) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  // no record read since Open, or the current one was deleted
  if (!btrieveDriver->hasCurrentRecord()) {
    return BtrieveError::InvalidPositioning;
  }

  if (*command.lpdwDataBufferLength < sizeof(uint32_t)) {
    return BtrieveError::DataBufferLengthOverrun;
  }

  *command.lpdwDataBufferLength = sizeof(uint32_t);
  *reinterpret_cast<uint32_t *>(command.lpDataBuffer) =
      btrieveDriver->getPosition();

  return BtrieveError::Success;
}

static BtrieveError GetDirectRecord(BtrieveCommand &command) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  // Get Direct replaces any logical currency with that of the key number
  // passed; key number -1 leaves none
  btrieveDriver->clearLogicalCurrency();

  if (*command.lpdwDataBufferLength < 4) {
    return BtrieveError::DataBufferLengthOverrun;
  }

  uint32_t position = *reinterpret_cast<uint32_t *>(command.lpDataBuffer);

  auto record = btrieveDriver->getRecord(position);
  if (!record.first) {
    return BtrieveError::InvalidRecordAddress;
  }

  if (command.keyNumber >= 0) {
    if (static_cast<uint32_t>(command.keyNumber) >=
        btrieveDriver->getKeys().size()) {
      return BtrieveError::InvalidKeyPath;
    }

    const auto &key = btrieveDriver->getKeys().at(command.keyNumber);
    if (command.lpKeyBufferLength < key.getLength()) {
      return BtrieveError::KeyBufferTooShort;
    }
  }

  // the record read becomes the current one (GetPosition, Update, Delete),
  // even when it does not fit the data buffer
  btrieveDriver->setPosition(position);

  if (command.keyNumber >= 0) {
    auto error =
        btrieveDriver->logicalCurrencySeek(command.keyNumber, position);
    if (error != BtrieveError::Success) {
      return error;
    }

    auto keyBytes =
        btrieveDriver->getKeys()
            .at(command.keyNumber)
            .extractKeyDataFromRecord(std::basic_string_view<uint8_t>(
                record.second.getData().data(),
                record.second.getData().size()));

    memcpy(command.lpKeyBuffer, keyBytes.data(), keyBytes.size());
  }

  // logical currency stays established when the record does not fit
  if (*command.lpdwDataBufferLength < record.second.getData().size()) {
    return BtrieveError::DataBufferLengthOverrun;
  }

  memcpy(command.lpDataBuffer, record.second.getData().data(),
         record.second.getData().size());
  *command.lpdwDataBufferLength = record.second.getData().size();

  return BtrieveError::Success;
}

// Like real Btrieve, a record longer than the caller's buffer still fills the
// buffer with as much as fits before returning status 22; some callers read
// the leading fields of long variable-length records that way rather than
// sizing their buffer to the whole record.
static BtrieveError copyRecordToDataBuffer(BtrieveCommand &command,
                                           const std::vector<uint8_t> &data) {
  const DWORD available = *command.lpdwDataBufferLength;
  const DWORD length = static_cast<DWORD>(
      std::min<size_t>(data.size(), static_cast<size_t>(available)));

  memcpy(command.lpDataBuffer, data.data(), length);
  *command.lpdwDataBufferLength = length;

  return data.size() > available ? BtrieveError::DataBufferLengthOverrun
                                 : BtrieveError::Success;
}

static BtrieveError Query(BtrieveCommand &command) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  std::basic_string_view<uint8_t> keyData;

  if (requiresKey(command.operation)) {
    if (static_cast<uint32_t>(command.keyNumber) >=
        btrieveDriver->getKeys().size()) {
      return BtrieveError::InvalidKeyNumber;
    } else if (command.lpKeyBufferLength <
               btrieveDriver->getKeys().at(command.keyNumber).getLength()) {
      return BtrieveError::KeyBufferTooShort;
    }

    keyData = std::basic_string_view<uint8_t>(
        reinterpret_cast<uint8_t *>(command.lpKeyBuffer),
        command.lpKeyBufferLength);
  }

  BtrieveError error = btrieveDriver->performOperation(
      command.keyNumber, keyData, command.operation);

  if (error != BtrieveError::Success) {
    return error;
  }

  auto record = btrieveDriver->getRecord();
  if (!record.first) {
    return requiresKey(command.operation) ? BtrieveError::KeyValueNotFound
                                          : BtrieveError::EndOfFile;
  }

  // always copy the key back to the client
  auto keyDataVector =
      btrieveDriver->getKeys()
          .at(command.keyNumber)
          .extractKeyDataFromRecord(std::basic_string_view<uint8_t>(
              record.second.getData().data(), record.second.getData().size()));

  memcpy(command.lpKeyBuffer, keyDataVector.data(), keyDataVector.size());

  if (acquiresData(command.operation)) {
    return copyRecordToDataBuffer(command, record.second.getData());
  }

  return BtrieveError::Success;
}

static BtrieveError Upsert(
    BtrieveCommand &command,
    std::function<std::pair<BtrieveError, unsigned int>(
        BtrieveDriver *, std::basic_string_view<uint8_t> record)>
        upsertFunction) {
  auto btrieveDriver = getOpenDatabase(command.lpPositionBlock);
  if (btrieveDriver == nullptr) {
    return BtrieveError::FileNotOpen;
  }

  // An Update's key number only picks the index to keep positioning on
  // afterwards; the record is the current one. Real Btrieve saves it even
  // when that key number is out of range, and some callers rely on that, so
  // do the same and just skip the repositioning, rather than failing before
  // the write and losing it.
  const bool keepPositioning =
      command.keyNumber >= 0 &&
      (command.operation != OperationCode::Update ||
       static_cast<uint32_t>(command.keyNumber) <
           btrieveDriver->getKeys().size());

  if (keepPositioning) {
    if (static_cast<uint32_t>(command.keyNumber) >=
        btrieveDriver->getKeys().size()) {
      return BtrieveError::InvalidKeyNumber;
    } else if (command.lpKeyBufferLength <
               btrieveDriver->getKeys().at(command.keyNumber).getLength()) {
      return BtrieveError::KeyBufferTooShort;
    }
  }

  auto record = std::basic_string_view<uint8_t>(
      reinterpret_cast<uint8_t *>(command.lpDataBuffer),
      *command.lpdwDataBufferLength);

  auto insertedPosition = upsertFunction(btrieveDriver, record);

  if (insertedPosition.first != BtrieveError::Success) {
    return insertedPosition.first;
  }

  // logicalCurrencySeek() below only establishes a query for continuing
  // Step operations against a specific key -- it doesn't update the
  // driver's tracked current position. Without this, a subsequent
  // GetPosition (which callers rely on immediately after Insert/Update to
  // learn the affected record's position) returns whatever position was
  // last set by a Step/Query, which can still be its unset default of 0
  // on a file that hasn't had one yet -- indistinguishable from failure.
  btrieveDriver->setPosition(insertedPosition.second);

  // Btrieve hands the stored record back on Insert, including any
  // autoincrement values it assigned
  if (command.operation == OperationCode::Insert) {
    auto stored = btrieveDriver->getRecord(insertedPosition.second);
    if (stored.first &&
        stored.second.getData().size() <= *command.lpdwDataBufferLength) {
      memcpy(command.lpDataBuffer, stored.second.getData().data(),
             stored.second.getData().size());
    }
  }

  if (!keepPositioning) {
    return BtrieveError::Success;
  }

  auto ret = btrieveDriver->logicalCurrencySeek(command.keyNumber,
                                                insertedPosition.second);

  if (ret == BtrieveError::Success) {
    // copy the key back to the client
    auto keyDataVector = btrieveDriver->getKeys()
                             .at(command.keyNumber)
                             .extractKeyDataFromRecord(record);

    memcpy(command.lpKeyBuffer, keyDataVector.data(), keyDataVector.size());
  }
  return ret;
}

// Btrieve's Version reply is 5 bytes: version (16-bit), revision (16-bit) and
// a product letter. Reports the 6.15 engine wbtrv32 stands in for. The
// product letter real 6.15 workstation engines return hasn't been checked
// against one.
static BtrieveError Version(BtrieveCommand &command) {
  constexpr DWORD replyLength = 5;
  if (command.lpdwDataBufferLength == nullptr ||
      *command.lpdwDataBufferLength < replyLength) {
    return BtrieveError::DataBufferLengthOverrun;
  }

  const uint16_t version = 6;
  const uint16_t revision = 15;
  uint8_t *reply = reinterpret_cast<uint8_t *>(command.lpDataBuffer);
  memcpy(reply, &version, sizeof(version));
  memcpy(reply + 2, &revision, sizeof(revision));
  reply[4] = 'W';
  *command.lpdwDataBufferLength = replyLength;
  return BtrieveError::Success;
}

static BtrieveError AbortTransaction(const BtrieveCommand &command);

static BtrieveError Stop(const BtrieveCommand &command) {
  if (_transactionActive) {
    ::AbortTransaction(command);
  }
  _openFiles.clear();

  return BtrieveError::Success;
}

// Begin Transaction (19, and its concurrent/locking variants).
static BtrieveError BeginTransaction(const BtrieveCommand &command) {
  if (_transactionActive) {
    return BtrieveError::TransactionIsActive;
  }
  _transactionActive = true;
  for (const auto &[guid, handle] : _openFiles) {
    joinTransaction(handle.driver);
  }
  return BtrieveError::Success;
}

static BtrieveError finishTransaction(bool commit) {
  if (!_transactionActive) {
    return BtrieveError::NoTransactionActive;
  }
  bool ok = true;
  for (const auto &driver : _transactionDrivers) {
    ok = (commit ? driver->endUserTransaction()
                 : driver->abortUserTransaction()) &&
         ok;
  }
  _transactionDrivers.clear();
  _transactionActive = false;
  return ok ? BtrieveError::Success : BtrieveError::TransactionError;
}

// End Transaction (20): commit every file's changes together.
static BtrieveError EndTransaction(const BtrieveCommand &) {
  return finishTransaction(true);
}

// Abort Transaction (21): undo every file's changes since Begin.
static BtrieveError AbortTransaction(const BtrieveCommand &) {
  return finishTransaction(false);
}

// Set Owner (29): give the open file an owner name. The name comes in the
// data and key buffers; the key number says what's allowed without it.
static BtrieveError SetOwner(BtrieveCommand &command) {
  OpenHandle *handle = const_cast<OpenHandle *>(findHandle(command.lpPositionBlock));
  if (handle == nullptr) {
    return BtrieveError::FileNotOpen;
  }
  OwnerInfo existing;
  if (handle->driver->readOwner(existing) && !existing.name.empty()) {
    return BtrieveError::OwnerAlreadySet;
  }
  if (command.keyNumber < 0 || command.keyNumber > 3) {
    return BtrieveError::InvalidOwner;
  }
  OwnerInfo owner;
  owner.name = suppliedOwnerName(command);
  owner.mode = command.keyNumber;
  if (owner.name.empty()) {
    return BtrieveError::InvalidOwner;
  }
  return handle->driver->writeOwner(&owner) ? BtrieveError::Success
                                            : BtrieveError::IOError;
}

// Clear Owner (30): remove the owner name, through a handle opened with it.
static BtrieveError ClearOwner(BtrieveCommand &command) {
  const OpenHandle *handle = findHandle(command.lpPositionBlock);
  if (handle == nullptr) {
    return BtrieveError::FileNotOpen;
  }
  if (!handle->ownerRights) {
    return BtrieveError::InvalidOwner;
  }
  return handle->driver->writeOwner(nullptr) ? BtrieveError::Success
                                             : BtrieveError::IOError;
}

// Set Directory (17): later relative file names are taken from this directory.
static BtrieveError SetDirectory(BtrieveCommand &command) {
  if (command.lpKeyBuffer == nullptr) {
    return BtrieveError::InvalidFileName;
  }
  const char *directory = reinterpret_cast<const char *>(command.lpKeyBuffer);
  wchar_t full[MAX_PATH];
  GetFullPathNameW(btrieve::toWideString(directory).c_str(), ARRAYSIZE(full),
                   full, nullptr);
  if (!std::filesystem::is_directory(full)) {
    return BtrieveError::InvalidFileName;
  }
  _btrieveDirectory = full;
  return BtrieveError::Success;
}

// Get Directory (18): the current directory, NUL-terminated, in the key
// buffer. (The key number names a drive; there is one current directory.)
static BtrieveError GetDirectory(BtrieveCommand &command) {
  wchar_t current[MAX_PATH];
  if (_btrieveDirectory.empty()) {
    GetCurrentDirectoryW(ARRAYSIZE(current), current);
  } else {
    wcscpy_s(current, _btrieveDirectory.c_str());
  }
  const std::string directory = btrieve::toStdString(current);
  if (command.lpKeyBuffer == nullptr ||
      command.lpKeyBufferLength < directory.size() + 1) {
    return BtrieveError::KeyBufferTooShort;
  }
  memcpy(command.lpKeyBuffer, directory.c_str(), directory.size() + 1);
  return BtrieveError::Success;
}

static BtrieveError Create(BtrieveCommand &command) {
  const char *lpszFileName =
      reinterpret_cast<const char *>(command.lpKeyBuffer);
  wchar_t fullPathFileName[MAX_PATH];
  SqliteDatabase sql;

  wbtrv32::LPFILESPEC lpFileSpec =
      reinterpret_cast<wbtrv32::LPFILESPEC>(command.lpDataBuffer);
  wbtrv32::LPKEYSPEC lpKeySpec =
      reinterpret_cast<wbtrv32::LPKEYSPEC>(lpFileSpec + 1);

  // special flag if we are to open in-memory
  const bool inMemory = (lpFileSpec->physicalPageSize == 0xFF);

  if (inMemory) {
    fullPathFileName[0] = 0;
  } else {
    resolveFileName(lpszFileName, fullPathFileName,
                    ARRAYSIZE(fullPathFileName));
  }

  std::filesystem::path dbPath = BtrieveDriver::sqlPathFor(
      std::filesystem::path(fullPathFileName), sql.getFileExtension());

  if (command.keyNumber == -1) {
    if (FileExists(fullPathFileName) ||
        FileExists(toWideString(dbPath).c_str())) {
      return BtrieveError::FileAlreadyExists;
    }
  }

  std::vector<wbtrv32::LPACSCREATEDATA> clientProvidedAcs;
  uint8_t numberOfAcs = 0;
  // find and categorize all the key data before populating everything
  for (uint16_t i = 0; i < lpFileSpec->numberOfKeys; ++i, ++lpKeySpec) {
  check_acs:
    if (lpKeySpec->attributes & NumberedACS) {
      numberOfAcs =
          std::max(numberOfAcs, static_cast<uint8_t>(lpKeySpec->acsNumber + 1));
    }

    if (lpKeySpec->attributes & SegmentedKey) {
      ++lpKeySpec;
      goto check_acs;
    }
  }
  // at this point we are pointing at the ACS data, so store it now
  wbtrv32::LPACSCREATEDATA lpAcsCreateData =
      reinterpret_cast<wbtrv32::LPACSCREATEDATA>(lpKeySpec);
  while (numberOfAcs--) {
    if (lpAcsCreateData->header != 0xAC) {
      return BtrieveError::InvalidACS;
    }

    clientProvidedAcs.push_back(lpAcsCreateData++);
  }

  lpKeySpec = reinterpret_cast<wbtrv32::LPKEYSPEC>(lpFileSpec + 1);
  std::vector<Key> keys;
  for (uint16_t i = 0; i < lpFileSpec->numberOfKeys; ++i, ++lpKeySpec) {
    char acsName[9];
    std::vector<char> acs;
    std::vector<KeyDefinition> keyDefinitions;

  process_key:
    if (lpKeySpec->attributes & NumberedACS) {
      wbtrv32::LPACSCREATEDATA lpAcsCreateData =
          clientProvidedAcs[lpKeySpec->acsNumber];

      memcpy(acsName, lpAcsCreateData->name, 8);
      acsName[8] = 0;

      acs.resize(ACS_LENGTH);
      memcpy(acs.data(), lpAcsCreateData->acs, ACS_LENGTH);
    } else {
      acsName[0] = 0;
    }

    KeyDefinition keyDefinition(
        i, static_cast<uint16_t>(lpKeySpec->length),
        static_cast<uint16_t>(lpKeySpec->position) - 1,
        static_cast<KeyDataType>(lpKeySpec->extendedDataType),
        lpKeySpec->attributes, lpKeySpec->attributes & SegmentedKey,
        lpKeySpec->attributes & SegmentedKey ? i : 0, 0, lpKeySpec->nullValue,
        acsName, acs);

    keyDefinitions.push_back(keyDefinition);

    if (lpKeySpec->attributes & SegmentedKey) {
      ++lpKeySpec;
      goto process_key;
    }

    keys.push_back(Key(keyDefinitions.data(), keyDefinitions.size()));
  }

  RecordType recordType = RecordType::Fixed;
  if (lpFileSpec->fileFlags & 2) {
    recordType = RecordType::VariableTruncated;
  } else if (lpFileSpec->fileFlags & 1) {
    recordType = RecordType::Variable;
  }

  BtrieveDatabase database(keys, static_cast<uint16_t>(lpFileSpec->pageSize), 0,
                           lpFileSpec->logicalFixedRecordLength,
                           lpFileSpec->logicalFixedRecordLength, 0, 0,
                           recordType, true, 0);

  if (!inMemory) {
    const std::filesystem::path datPath(fullPathFileName);
    {
      // finish the creation transaction so the identity below can be written
      auto recordLoader = sql.create(toWideString(dbPath).c_str(), database);
      recordLoader->onRecordsComplete();
    }
    sql.close();

    // Real Btrieve leaves the named file on disk, and callers check for it
    // with ordinary file APIs ("create it unless it already exists"), so put
    // a marker for the new database under that name.
    if (datPath != dbPath) {
      const std::string id = BtrieveDriver::newDatabaseId();
      sql.writeIdentity(dbPath, {id, datPath.filename().string()});
      BtrieveDriver::writeMarker(datPath, dbPath, id);
    }
    return BtrieveError::Success;
  }

  // this is the in-memory create path
  std::unique_ptr<SqliteDatabase> sqliteDatabase(new SqliteDatabase());
  std::unique_ptr<RecordLoader> recordLoader =
      sqliteDatabase->create(nullptr, database);
  // create() leaves a transaction open (via createSqliteInsertionCommand)
  // for callers that are about to bulk load records; since we have none to
  // load, commit it now so later Insert/Update/etc. calls on this in-memory
  // database aren't stuck behind an unclosed transaction.
  recordLoader->onRecordsComplete();

  AddToOpenFiles(command,
                 std::make_shared<BtrieveDriver>(sqliteDatabase.release()),
                 true, OpenMode::Normal);

  return BtrieveError::Success;
}

#define CASE_WITH_RECORD_LOCK(a)   \
  case a:                          \
  case a##_SingleWaitRecordLock:   \
  case a##_SingleNoWaitRecordLock: \
  case a##_MultipleWaitRecordLock: \
  case a##_MultipleNoWaitRecordLock

// clang-format off
// --- Extended operations (Get/Step Next/Previous Extended, Insert Extended) ---
//
// The data buffer holds a descriptor:
//   header     uint16 descriptor length, char[2] "EG" (start with the next
//              record) or "UC" (start with the current one)
//   filter     uint16 maximum reject count (0 = 4095), uint16 term count,
//              then each term: uint8 type, uint16 length, uint16 offset,
//              uint8 comparison (1 = 2 > 3 < 4 <> 5 >= 6 <=, +32 compare with
//              the file's alternate collating sequence, +64 compare with
//              another field whose offset follows instead of a value, +128
//              case-insensitive), uint8 connector (0 last, 1 AND, 2 OR), then
//              the value (length bytes) or a uint16 field offset
//   extractor  uint16 records wanted, uint16 field count, then each field:
//              uint16 length, uint16 offset
// and receives: uint16 record count, then per record uint16 length, uint32
// position and the extracted bytes. Terms are evaluated left to right.

namespace {
struct FilterTerm {
  uint8_t type;
  uint16_t length;
  uint16_t offset;
  uint8_t comparison;
  uint8_t connector;
  std::vector<uint8_t> value;  // empty when comparing with another field
  uint16_t otherOffset;
};

struct ExtractField {
  uint16_t length;
  uint16_t offset;
};

struct ExtendedDescriptor {
  bool startWithCurrent;
  uint16_t maxRejects;
  std::vector<FilterTerm> terms;
  uint16_t recordsWanted;
  std::vector<ExtractField> fields;
};

uint16_t readU16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

bool parseDescriptor(const uint8_t *buffer, DWORD bufferLength,
                     ExtendedDescriptor &descriptor) {
  if (bufferLength < 8) {
    return false;
  }
  const uint16_t length = readU16(buffer);
  if (length < 8 || length > bufferLength) {
    return false;
  }
  if (memcmp(buffer + 2, "EG", 2) == 0) {
    descriptor.startWithCurrent = false;
  } else if (memcmp(buffer + 2, "UC", 2) == 0) {
    descriptor.startWithCurrent = true;
  } else {
    return false;
  }
  size_t at = 4;
  auto need = [&](size_t bytes) { return at + bytes <= length; };

  if (!need(4)) return false;
  descriptor.maxRejects = readU16(buffer + at);
  if (descriptor.maxRejects == 0) {
    descriptor.maxRejects = 4095;
  }
  const uint16_t termCount = readU16(buffer + at + 2);
  at += 4;
  for (uint16_t i = 0; i < termCount; ++i) {
    if (!need(7)) return false;
    FilterTerm term;
    term.type = buffer[at];
    term.length = readU16(buffer + at + 1);
    term.offset = readU16(buffer + at + 3);
    term.comparison = buffer[at + 5];
    term.connector = buffer[at + 6];
    at += 7;
    const uint8_t code = term.comparison & 0x1F;
    if (code < 1 || code > 6 || term.connector > 2) return false;
    if (term.comparison & 64) {
      if (!need(2)) return false;
      term.otherOffset = readU16(buffer + at);
      at += 2;
    } else {
      if (!need(term.length)) return false;
      term.value.assign(buffer + at, buffer + at + term.length);
      term.otherOffset = 0;
      at += term.length;
    }
    descriptor.terms.push_back(std::move(term));
  }
  if (!descriptor.terms.empty() && descriptor.terms.back().connector != 0) {
    return false;  // the last term must say it's the last
  }

  if (!need(4)) return false;
  descriptor.recordsWanted = readU16(buffer + at);
  const uint16_t fieldCount = readU16(buffer + at + 2);
  at += 4;
  if (descriptor.recordsWanted == 0 || fieldCount == 0) return false;
  for (uint16_t i = 0; i < fieldCount; ++i) {
    if (!need(4)) return false;
    descriptor.fields.push_back({readU16(buffer + at), readU16(buffer + at + 2)});
    at += 4;
  }
  return true;
}

// <0, 0, >0 comparing two fields of the given Btrieve type and length.
int compareField(uint8_t type, const uint8_t *a, const uint8_t *b,
                 uint16_t length, bool caseInsensitive, const char *acs) {
  auto compareBytes = [&](const uint8_t *x, size_t xLength, const uint8_t *y,
                          size_t yLength) {
    const size_t n = std::min(xLength, yLength);
    for (size_t i = 0; i < n; ++i) {
      int cx = x[i], cy = y[i];
      if (acs != nullptr) {
        cx = static_cast<uint8_t>(acs[cx]);
        cy = static_cast<uint8_t>(acs[cy]);
      } else if (caseInsensitive) {
        cx = toupper(cx);
        cy = toupper(cy);
      }
      if (cx != cy) return cx < cy ? -1 : 1;
    }
    return xLength == yLength ? 0 : (xLength < yLength ? -1 : 1);
  };
  auto littleEndian = [&](const uint8_t *p) {
    uint64_t value = 0;
    for (int i = std::min<int>(length, 8) - 1; i >= 0; --i) {
      value = value << 8 | p[i];
    }
    return value;
  };

  switch (type) {
    case KeyDataType::Integer:
    case KeyDataType::AutoInc: {
      // sign-extend from the field's length
      const int bits = std::min<int>(length, 8) * 8;
      auto signedValue = [&](const uint8_t *p) {
        uint64_t value = littleEndian(p);
        if (bits < 64 && (value >> (bits - 1)) & 1) {
          value |= ~uint64_t{0} << bits;
        }
        return static_cast<int64_t>(value);
      };
      const int64_t x = signedValue(a), y = signedValue(b);
      return x == y ? 0 : (x < y ? -1 : 1);
    }
    case KeyDataType::Unsigned:
    case KeyDataType::UnsignedBinary:
    case KeyDataType::Logical:
    case KeyDataType::Date:  // day, month, uint16 year: little-endian order
    case KeyDataType::Time:  // hundredths, seconds, minutes, hours
    case KeyDataType::OldBinary: {
      const uint64_t x = littleEndian(a), y = littleEndian(b);
      return x == y ? 0 : (x < y ? -1 : 1);
    }
    case KeyDataType::Float: {
      double x, y;
      if (length == 4) {
        float fx, fy;
        memcpy(&fx, a, 4);
        memcpy(&fy, b, 4);
        x = fx;
        y = fy;
      } else if (length == 8) {
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
      } else {
        return compareBytes(a, length, b, length);
      }
      return x == y ? 0 : (x < y ? -1 : 1);
    }
    case KeyDataType::Zstring:
      return compareBytes(a, strnlen(reinterpret_cast<const char *>(a), length),
                          b, strnlen(reinterpret_cast<const char *>(b), length));
    case KeyDataType::Lstring: {
      const size_t xLength = std::min<size_t>(a[0], length - 1u);
      const size_t yLength = std::min<size_t>(b[0], length - 1u);
      return compareBytes(a + 1, xLength, b + 1, yLength);
    }
    default:  // strings and everything compared byte for byte
      return compareBytes(a, length, b, length);
  }
}

bool termMatches(const FilterTerm &term, const std::vector<uint8_t> &record,
                 const char *acs) {
  if (static_cast<size_t>(term.offset) + term.length > record.size()) {
    return false;
  }
  const uint8_t *field = record.data() + term.offset;
  const uint8_t *other;
  if (term.comparison & 64) {
    if (static_cast<size_t>(term.otherOffset) + term.length > record.size()) {
      return false;
    }
    other = record.data() + term.otherOffset;
  } else {
    other = term.value.data();
  }
  const int order =
      compareField(term.type, field, other, term.length,
                   (term.comparison & 128) != 0,
                   (term.comparison & 32) != 0 ? acs : nullptr);
  switch (term.comparison & 0x1F) {
    case 1: return order == 0;
    case 2: return order > 0;
    case 3: return order < 0;
    case 4: return order != 0;
    case 5: return order >= 0;
    case 6: return order <= 0;
  }
  return false;
}

bool recordMatches(const ExtendedDescriptor &descriptor,
                   const std::vector<uint8_t> &record, const char *acs) {
  if (descriptor.terms.empty()) {
    return true;
  }
  bool result = termMatches(descriptor.terms[0], record, acs);
  for (size_t i = 1; i < descriptor.terms.size(); ++i) {
    const bool next = termMatches(descriptor.terms[i], record, acs);
    result = descriptor.terms[i - 1].connector == 1 ? (result && next)
                                                    : (result || next);
  }
  return result;
}
}  // namespace

static BtrieveError ExtendedGet(BtrieveCommand &command, bool byKey,
                                bool forward) {
  BtrieveDriver *driver = getOpenDatabase(command.lpPositionBlock);
  if (driver == nullptr) {
    return BtrieveError::FileNotOpen;
  }
  if (command.lpDataBuffer == nullptr ||
      command.lpdwDataBufferLength == nullptr) {
    return BtrieveError::IncorrectDescriptor;
  }
  uint8_t *buffer = reinterpret_cast<uint8_t *>(command.lpDataBuffer);
  const DWORD bufferLength = *command.lpdwDataBufferLength;

  ExtendedDescriptor descriptor;
  if (!parseDescriptor(buffer, bufferLength, descriptor)) {
    return BtrieveError::IncorrectDescriptor;
  }
  if (byKey && (command.keyNumber < 0 ||
                static_cast<size_t>(command.keyNumber) >=
                    driver->getKeys().size())) {
    return BtrieveError::InvalidKeyNumber;
  }

  // an ACS for "+32" comparisons: the first one the file's keys use
  const char *acs = nullptr;
  for (const auto &key : driver->getKeys()) {
    for (const auto &segment : key.getSegments()) {
      if (acs == nullptr && segment.getACS() != nullptr) {
        acs = segment.getACS();
      }
    }
  }

  const OperationCode move =
      byKey ? (forward ? OperationCode::AcquireNext
                       : OperationCode::AcquirePrevious)
            : (forward ? OperationCode::StepNext : OperationCode::StepPrevious);

  std::vector<uint8_t> output(2, 0);
  uint16_t returned = 0;
  uint16_t rejected = 0;
  std::vector<uint8_t> lastRecord;
  BtrieveError status = BtrieveError::Success;
  bool first = true;

  while (true) {
    if (!(first && descriptor.startWithCurrent)) {
      BtrieveError moved = driver->performOperation(
          byKey ? command.keyNumber : 0, std::basic_string_view<uint8_t>(),
          move);
      if (moved != BtrieveError::Success) {
        status = moved == BtrieveError::KeyValueNotFound
                     ? BtrieveError::EndOfFile
                     : moved;
        break;
      }
    }
    first = false;

    auto record = driver->getRecord();
    if (!record.first) {
      status = BtrieveError::EndOfFile;
      break;
    }
    const std::vector<uint8_t> &data = record.second.getData();
    lastRecord = data;

    if (!recordMatches(descriptor, data, acs)) {
      if (++rejected >= descriptor.maxRejects) {
        status = BtrieveError::RejectCountReached;
        break;
      }
      continue;
    }

    // extract the requested fields
    std::vector<uint8_t> extracted;
    for (const auto &field : descriptor.fields) {
      if (static_cast<size_t>(field.offset) + field.length > data.size()) {
        status = BtrieveError::IncorrectFieldOffset;
        break;
      }
      extracted.insert(extracted.end(), data.begin() + field.offset,
                       data.begin() + field.offset + field.length);
    }
    if (status != BtrieveError::Success) {
      break;
    }
    if (output.size() + 6 + extracted.size() > bufferLength) {
      status = BtrieveError::DataBufferLengthOverrun;
      break;
    }
    const uint16_t length = static_cast<uint16_t>(extracted.size());
    const uint32_t position = driver->getPosition();
    output.push_back(static_cast<uint8_t>(length));
    output.push_back(static_cast<uint8_t>(length >> 8));
    for (int i = 0; i < 4; ++i) {
      output.push_back(static_cast<uint8_t>(position >> (8 * i)));
    }
    output.insert(output.end(), extracted.begin(), extracted.end());
    rejected = 0;
    if (++returned == descriptor.recordsWanted) {
      break;
    }
  }

  output[0] = static_cast<uint8_t>(returned);
  output[1] = static_cast<uint8_t>(returned >> 8);
  memcpy(buffer, output.data(), output.size());
  *command.lpdwDataBufferLength = static_cast<DWORD>(output.size());

  // the key buffer gets the key of the last record looked at
  if (byKey && !lastRecord.empty() && command.lpKeyBuffer != nullptr) {
    auto key = driver->getKeys()
                   .at(command.keyNumber)
                   .extractKeyDataFromRecord(std::basic_string_view<uint8_t>(
                       lastRecord.data(), lastRecord.size()));
    if (key.size() <= command.lpKeyBufferLength) {
      memcpy(command.lpKeyBuffer, key.data(), key.size());
    }
  }
  return status;
}

// Insert Extended (40): data buffer = uint16 record count, then each record
// as uint16 length and its bytes. Returns uint16 count inserted and each new
// record's uint32 position. Stops at the first record that fails.
static BtrieveError InsertExtended(BtrieveCommand &command) {
  BtrieveDriver *driver = getOpenDatabase(command.lpPositionBlock);
  if (driver == nullptr) {
    return BtrieveError::FileNotOpen;
  }
  if (command.lpDataBuffer == nullptr ||
      command.lpdwDataBufferLength == nullptr ||
      *command.lpdwDataBufferLength < 2) {
    return BtrieveError::IncorrectDescriptor;
  }
  uint8_t *buffer = reinterpret_cast<uint8_t *>(command.lpDataBuffer);
  const DWORD bufferLength = *command.lpdwDataBufferLength;
  const std::vector<uint8_t> input(buffer, buffer + bufferLength);

  const uint16_t count = readU16(input.data());
  std::vector<uint32_t> positions;
  size_t at = 2;
  BtrieveError status = BtrieveError::Success;
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 2 > input.size()) {
      status = BtrieveError::IncorrectDescriptor;
      break;
    }
    const uint16_t length = readU16(input.data() + at);
    at += 2;
    if (at + length > input.size()) {
      status = BtrieveError::IncorrectDescriptor;
      break;
    }
    auto inserted = driver->insertRecord(
        std::basic_string_view<uint8_t>(input.data() + at, length));
    at += length;
    if (inserted.first != BtrieveError::Success) {
      status = inserted.first;
      break;
    }
    positions.push_back(inserted.second);
    driver->setPosition(inserted.second);
  }

  std::vector<uint8_t> output(2 + 4 * positions.size());
  output[0] = static_cast<uint8_t>(positions.size());
  output[1] = static_cast<uint8_t>(positions.size() >> 8);
  for (size_t i = 0; i < positions.size(); ++i) {
    for (int b = 0; b < 4; ++b) {
      output[2 + 4 * i + b] = static_cast<uint8_t>(positions[i] >> (8 * b));
    }
  }
  if (output.size() <= bufferLength) {
    memcpy(buffer, output.data(), output.size());
    *command.lpdwDataBufferLength = static_cast<DWORD>(output.size());
  }
  return status;
}

// The handle a position block belongs to, without making it the active one.
static const OpenHandle *findHandle(LPVOID lpPositionBlock) {
  if (lpPositionBlock == nullptr) {
    return nullptr;
  }
  wchar_t guidStr[64];
  StringFromGUID2(*reinterpret_cast<GUID *>(lpPositionBlock), guidStr,
                  ARRAYSIZE(guidStr));
  auto iterator = _openFiles.find(std::basic_string<wchar_t>(guidStr));
  return iterator == _openFiles.end() ? nullptr : &iterator->second;
}

static BtrieveError handle(BtrieveCommand &command) {
  // Begin Transaction also comes with lock biases (+100..+400) and the
  // concurrent bias (+1000); they all begin a transaction here
  if (static_cast<int>(command.operation) % 100 == 19) {
    command.operation = OperationCode::BeginTransaction;
  }

  // writes through a handle opened read-only are refused, as Btrieve does
  if (command.operation == OperationCode::Insert ||
      command.operation == OperationCode::InsertExtended ||
      command.operation == OperationCode::Update ||
      command.operation == OperationCode::Delete) {
    const OpenHandle *handle = findHandle(command.lpPositionBlock);
    if (handle != nullptr && handle->mode == OpenMode::ReadOnly) {
      return BtrieveError::AccessDenied;
    }
  }

  switch (command.operation) {
    case OperationCode::Open:
      return ::Open(command);
    case OperationCode::Close:
      return ::Close(command);
    case OperationCode::Stat:
      return ::Stat(command);
    case OperationCode::Delete:
      return ::Delete(command);
    CASE_WITH_RECORD_LOCK(OperationCode::StepFirst):
    CASE_WITH_RECORD_LOCK(OperationCode::StepLast):
    CASE_WITH_RECORD_LOCK(OperationCode::StepNext):
    CASE_WITH_RECORD_LOCK(OperationCode::StepPrevious):
      return ::Step(command);
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireFirst):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireLast):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireNext):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquirePrevious):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireEqual):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireGreater):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireGreaterOrEqual):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireLess):
    CASE_WITH_RECORD_LOCK(OperationCode::AcquireLessOrEqual):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryFirst):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryLast):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryNext):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryPrevious):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryEqual):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryGreater):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryGreaterOrEqual):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryLess):
    CASE_WITH_RECORD_LOCK(OperationCode::QueryLessOrEqual):
      return ::Query(command);
    case OperationCode::GetPosition:
      return ::GetPosition(command);
    CASE_WITH_RECORD_LOCK(OperationCode::GetDirectChunkOrRecord):
      return ::GetDirectRecord(command);
    case OperationCode::Update:
      return ::Upsert(command, [](BtrieveDriver *driver,
                                  std::basic_string_view<uint8_t> record) {
        auto position = driver->getPosition();
        return std::make_pair(driver->updateRecord(position, record), position);
      });
    case OperationCode::Insert:
      return ::Upsert(command, [](BtrieveDriver *driver,
                                  std::basic_string_view<uint8_t> record) {
        return driver->insertRecord(record);
      });
    case OperationCode::Stop:
      return ::Stop(command);
    case OperationCode::Version:
      return ::Version(command);
    case OperationCode::SetDirectory:
      return ::SetDirectory(command);
    case OperationCode::SetOwner:
      return ::SetOwner(command);
    CASE_WITH_RECORD_LOCK(OperationCode::GetNextExtended):
      return ::ExtendedGet(command, true, true);
    CASE_WITH_RECORD_LOCK(OperationCode::GetPreviousExtended):
      return ::ExtendedGet(command, true, false);
    CASE_WITH_RECORD_LOCK(OperationCode::StepNextExtended):
      return ::ExtendedGet(command, false, true);
    CASE_WITH_RECORD_LOCK(OperationCode::StepPreviousExtended):
      return ::ExtendedGet(command, false, false);
    case OperationCode::InsertExtended:
      return ::InsertExtended(command);
    case OperationCode::ClearOwner:
      return ::ClearOwner(command);
    CASE_WITH_RECORD_LOCK(OperationCode::BeginTransaction):
    CASE_WITH_RECORD_LOCK(OperationCode::BeginConcurrentTransaction):
    CASE_WITH_RECORD_LOCK(OperationCode::BeginConcurrentNoWaitTransaction):
      return ::BeginTransaction(command);
    case OperationCode::EndTransaction:
      return ::EndTransaction(command);
    case OperationCode::AbortTransaction:
      return ::AbortTransaction(command);
    case OperationCode::GetDirectory:
      return ::GetDirectory(command);
    // Record locks are accepted and never contended (one process owns the
    // files), so there is never anything to unlock.
    case OperationCode::Unlock:
    // Extend spreads a file over a second disk; one SQLite file has no use for
    // that, and the file's data is unaffected either way.
    case OperationCode::Extend:
      return BtrieveError::Success;
    // Reset: give up everything this client holds, as Stop does.
    case OperationCode::Reset:
      return ::Stop(command);
    case OperationCode::Create:
      return ::Create(command);
    default:
      return BtrieveError::InvalidOperation;
  }
}
// clang-format on

#if defined(WIN32) && defined(__i386__) && !defined(_MSC_VER)
// Export the 32-bit stdcall symbol _BTRCALL@28 under its undecorated name,
// matching wbtrv32.def in the MSVC build; see WBTRV32_EXPORT in framework.h.
__asm__(".section .drectve\n.ascii \" -export:BTRCALL=BTRCALL@28\"\n.text");
#endif

extern "C" WBTRV32_EXPORT int __stdcall BTRCALL(WORD wOperation,
                                                LPVOID lpPositionBlock,
                                                LPVOID lpDataBuffer,
                                                LPDWORD lpdwDataBufferLength,
                                                LPVOID lpKeyBuffer,
                                                BYTE bKeyLength,
                                                CHAR sbKeyNumber) {
  // One call at a time, as the real Btrieve requester does: the table of open
  // handles, each handle's position swap and the log are all shared state, and
  // a host may call from more than one thread (server threads, utilities).
  static std::mutex callLock;
  std::lock_guard<std::mutex> lock(callLock);

  BtrieveCommand btrieveCommand;
  BtrieveError error;

  // Btrieve 6.15 uses only the low word of the data length. The Major BBS
  // relies on this: it passes the address of a 16-bit length as the 32-bit
  // pointer (DFAAPI.C btvu), so the high word is whatever was next to it.
  if (lpdwDataBufferLength != nullptr) {
    *lpdwDataBufferLength &= 0xFFFF;
  }

  btrieveCommand.operation = static_cast<OperationCode>(wOperation);
  btrieveCommand.lpPositionBlock = lpPositionBlock;
  btrieveCommand.lpDataBuffer = lpDataBuffer;
  btrieveCommand.lpdwDataBufferLength = lpdwDataBufferLength;
  btrieveCommand.lpKeyBuffer = lpKeyBuffer;
  btrieveCommand.lpKeyBufferLength = bKeyLength;
  btrieveCommand.keyNumber = sbKeyNumber;

  try {
    error = handle(btrieveCommand);
  } catch (const BtrieveException &ex) {
    // make sure we don't leak the exception back to our caller
    error = ex.getError();
    debug(btrieveCommand, "%s failed: %s", toString(btrieveCommand.operation),
          ex.getErrorMessage().c_str());
  } catch (...) {
    // BtrieveException doesn't derive from std::exception, so a stray
    // std::exception (e.g. std::out_of_range from a malformed record) would
    // otherwise unwind straight through this extern "C" boundary -- make
    // sure nothing ever escapes back to our (possibly non-C++) caller.
    error = BtrieveError::UnrecoverableError;
  }

  // Not found, end of file and a partial read into a short buffer are how
  // applications ask questions, not problems; by default they go unlogged.
  const bool routine = error == BtrieveError::KeyValueNotFound ||
                       error == BtrieveError::EndOfFile ||
                       error == BtrieveError::DataBufferLengthOverrun;
  if (error != BtrieveError::Success &&
      _logLevel >= (routine ? LogAllStatuses : LogProblems)) {
    debug(btrieveCommand, "handled %s [key %d], returned %s",
          toString(btrieveCommand.operation), btrieveCommand.keyNumber,
          errorToString(error));
  }

  return error;
}

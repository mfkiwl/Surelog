/*
  Copyright 2019 Alain Dargelas

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
 * File:   Cache.cpp
 * Author: alain
 *
 * Created on April 28, 2017, 9:32 PM
 */

#include <Surelog/Cache/Cache.h>
#include <Surelog/CommandLine/CommandLineParser.h>
#include <Surelog/Common/FileSystem.h>
#include <Surelog/Design/FileContent.h>
#include <Surelog/ErrorReporting/ErrorContainer.h>
#include <Surelog/SourceCompile/SymbolTable.h>
#include <flatbuffers/util.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <filesystem>
#include <iostream>

namespace SURELOG {
static constexpr std::string_view UnknownRawPath = "<unknown>";

std::string_view Cache::getExecutableTimeStamp() const {
  static constexpr std::string_view sExecTstamp(__DATE__ "-" __TIME__);
  return sExecTstamp;
}

bool Cache::openFlatBuffers(PathId cacheFileId,
                            std::vector<char>& content) const {
  FileSystem* const fileSystem = FileSystem::getInstance();
  return fileSystem->loadContent(cacheFileId, content);
}

bool Cache::checkIfCacheIsValid(const SURELOG::CACHE::Header* header,
                                std::string_view schemaVersion,
                                PathId cacheFileId, PathId sourceFileId) const {
  /* Schema version */
  if (schemaVersion != header->flb_version()->string_view()) {
    return false;
  }

  /* Tool version */
  if (CommandLineParser::getVersionNumber() !=
      header->sl_version()->string_view()) {
    return false;
  }

  /* Timestamp Tool that created Cache vs tool date */
  if (getExecutableTimeStamp() != header->sl_date_compiled()->string_view()) {
    return false;
  }

  /* Timestamp Cache vs Orig File */
  if (cacheFileId && sourceFileId) {
    FileSystem* const fileSystem = FileSystem::getInstance();
    std::filesystem::file_time_type ct = fileSystem->modtime(cacheFileId);
    std::filesystem::file_time_type ft = fileSystem->modtime(sourceFileId);

    if (ft == std::filesystem::file_time_type::min()) {
      return false;
    }
    if (ct == std::filesystem::file_time_type::min()) {
      return false;
    }
    if (ct < ft) {
      return false;
    }
  }
  return true;
}

flatbuffers::Offset<SURELOG::CACHE::Header> Cache::createHeader(
    flatbuffers::FlatBufferBuilder& builder, std::string_view schemaVersion) {
  auto sl_version = builder.CreateString(CommandLineParser::getVersionNumber());
  auto sl_build_date = builder.CreateString(getExecutableTimeStamp());
  auto sl_flb_version = builder.CreateString(schemaVersion);
  auto header =
      CACHE::CreateHeader(builder, sl_version, sl_flb_version, sl_build_date);
  return header;
}

bool Cache::saveFlatbuffers(const flatbuffers::FlatBufferBuilder& builder,
                            PathId cacheFileId, SymbolTable* symbolTable) {
  FileSystem* const fileSystem = FileSystem::getInstance();

  const unsigned char* buf = builder.GetBufferPointer();
  const int32_t size = builder.GetSize();

  PathId cacheDirId = fileSystem->getParent(cacheFileId, symbolTable);
  return fileSystem->mkdirs(cacheDirId) &&
         fileSystem->saveContent(
             cacheFileId, reinterpret_cast<const char*>(buf), size, true);
}

flatbuffers::Offset<Cache::VectorOffsetError> Cache::cacheErrors(
    flatbuffers::FlatBufferBuilder& builder, SymbolTable* cacheSymbols,
    const ErrorContainer* errorContainer, const SymbolTable& localSymbols,
    PathId subjectId) {
  FileSystem* const fileSystem = FileSystem::getInstance();
  const std::vector<Error>& errors = errorContainer->getErrors();
  std::vector<flatbuffers::Offset<SURELOG::CACHE::Error>> error_vec;
  for (const Error& error : errors) {
    const std::vector<Location>& locs = error.getLocations();
    if (!locs.empty()) {
      bool matchSubject = false;
      for (const Location& loc : locs) {
        if (loc.m_fileId == subjectId) {
          matchSubject = true;
          break;
        }
      }
      if (matchSubject) {
        std::vector<flatbuffers::Offset<SURELOG::CACHE::Location>> location_vec;
        for (const Location& loc : locs) {
          PathId canonicalFileId = fileSystem->copy(loc.m_fileId, cacheSymbols);
          SymbolId canonicalObjectId =
              cacheSymbols->copyFrom(loc.m_object, &localSymbols);
          auto locflb = CACHE::CreateLocation(
              builder, (RawPathId)canonicalFileId, loc.m_line, loc.m_column,
              (RawSymbolId)canonicalObjectId);
          location_vec.push_back(locflb);
        }
        auto locvec = builder.CreateVector(location_vec);
        auto err = CACHE::CreateError(builder, locvec, error.getType());
        error_vec.push_back(err);
      }
    }
  }

  return builder.CreateVector(error_vec);
}

void Cache::restoreSymbols(const VectorOffsetString* symbolsBuf,
                           SymbolTable* cacheSymbols) {
  for (uint32_t i = 0; i < symbolsBuf->size(); i++) {
    std::string_view symbol = symbolsBuf->Get(i)->string_view();
    cacheSymbols->registerSymbol(symbol);
  }
}

void Cache::restoreErrors(const VectorOffsetError* errorsBuf,
                          SymbolTable* cacheSymbols,
                          ErrorContainer* errorContainer,
                          SymbolTable* localSymbols) {
  FileSystem* const fileSystem = FileSystem::getInstance();
  for (uint32_t i = 0; i < errorsBuf->size(); i++) {
    auto errorFlb = errorsBuf->Get(i);
    std::vector<Location> locs;
    for (uint32_t j = 0; j < errorFlb->locations()->size(); j++) {
      auto locFlb = errorFlb->locations()->Get(j);
      PathId translFileId = fileSystem->toPathId(
          fileSystem->remap(cacheSymbols->getSymbol(
              SymbolId(locFlb->file_id(), UnknownRawPath))),
          localSymbols);
      SymbolId translObjectId = localSymbols->copyFrom(
          SymbolId(locFlb->object(), UnknownRawPath), cacheSymbols);
      locs.emplace_back(translFileId, locFlb->line(), locFlb->column(),
                        translObjectId);
    }
    Error err((ErrorDefinition::ErrorType)errorFlb->error_id(), locs);
    errorContainer->addError(err, false);
  }
}

std::vector<CACHE::VObject> Cache::cacheVObjects(
    const FileContent* fcontent, SymbolTable* cacheSymbols,
    const SymbolTable& localSymbols, PathId fileId) {
  /* Cache the design objects */
  // std::vector<flatbuffers::Offset<PARSECACHE::VObject>> object_vec;
  std::vector<CACHE::VObject> object_vec;
  if (!fcontent) return object_vec;
  if (fcontent->getVObjects().size() > Capacity) {
    std::cout << "INTERNAL ERROR: Cache is saturated, Use -nocache option\n";
    return object_vec;
  }
  // Convert a local symbol ID to a cache symbol ID to be stored.
  std::function<uint64_t(SymbolId)> toCacheSym = [cacheSymbols,
                                                  &localSymbols](SymbolId id) {
    return (RawSymbolId)cacheSymbols->copyFrom(id, &localSymbols);
  };
  std::function<uint64_t(PathId)> toCachePath = [cacheSymbols](PathId id) {
    FileSystem* const fileSystem = FileSystem::getInstance();
    return (RawPathId)fileSystem->copy(id, cacheSymbols);
  };

  for (const VObject& object : fcontent->getVObjects()) {
    // Lets compress this struct into 20 and 16 bits fields:
    //  object_vec.push_back(PARSECACHE::CreateVObject(builder,
    //                                                 toCacheSym(object.m_name),
    //                                                 object.m_uniqueId,
    //                                                 object.m_type,
    //                                                 object.m_column,
    //                                                 object.m_line,
    //                                                 object.m_parent,
    //                                                 object.m_definition,
    //                                                 object.m_child,
    //                                                 object.m_sibling));

    uint64_t field1 = 0;
    uint64_t field2 = 0;
    uint64_t field3 = 0;
    uint64_t field4 = 0;

    // clang-format off
    field1 |= 0x0000000000FFFFFF & toCacheSym(object.m_name);
    field1 |= 0x0000000FFF000000 & (((uint64_t)object.m_type)                  << (24));
    field1 |= 0x0000FFF000000000 & (((uint64_t)object.m_column)                << (24 + 12));
    field1 |= 0xFFFF000000000000 & (((uint64_t)(RawNodeId)object.m_parent)     << (24 + 12 + 12));
    field2 |= 0x0000000000000FFF & (((uint64_t)(RawNodeId)object.m_parent)     >> (16));
    field2 |= 0x000000FFFFFFF000 & (((uint64_t)(RawNodeId)object.m_definition) << (12));
    field2 |= 0xFFFFFF0000000000 & (((uint64_t)(RawNodeId)object.m_child)      << (12 + 28));
    field3 |= 0x000000000000000F & (((uint64_t)(RawNodeId)object.m_child)      >> (24));
    field3 |= 0x00000000FFFFFFF0 & (((uint64_t)(RawNodeId)object.m_sibling)    << (4));
    field3 |= 0x00FFFFFF00000000 & (toCachePath(object.m_fileId)               << (4 + 28));
    field3 |= 0xFF00000000000000 & (((uint64_t)object.m_line)                  << (4 + 28 + 24));
    field4 |= 0x000000000000FFFF & (((uint64_t)object.m_line)                  >> (8));
    field4 |= 0x000000FFFFFF0000 & (((uint64_t)object.m_endLine)               << (16));
    field4 |= 0x000FFF0000000000 & (((uint64_t)object.m_endColumn)             << (16 + 24));
    // clang-format on

    object_vec.emplace_back(field1, field2, field3, field4);
  }

  return object_vec;
}

void Cache::restoreVObjects(
    const flatbuffers::Vector<const SURELOG::CACHE::VObject*>* objects,
    const SymbolTable& cacheSymbols, SymbolTable* localSymbols, PathId fileId,
    FileContent* fileContent) {
  restoreVObjects(objects, cacheSymbols, localSymbols, fileId,
                  fileContent->mutableVObjects());
}

void Cache::restoreVObjects(
    const flatbuffers::Vector<const SURELOG::CACHE::VObject*>* objects,
    const SymbolTable& cacheSymbols, SymbolTable* localSymbols, PathId fileId,
    std::vector<VObject>* result) {
  FileSystem* const fileSystem = FileSystem::getInstance();
  /* Restore design objects */
  result->clear();
  result->reserve(objects->size());
  for (const auto* objectc : *objects) {
    // VObject object
    // (m_parse->getCompileSourceFile()->getSymbolTable()->registerSymbol(canonicalSymbols.getSymbol(objectc->m_name())),
    //                (VObjectType) objectc->m_type(), objectc->m_uniqueId(),
    //                objectc->m_column(), objectc->m_line(),
    //                objectc->m_parent(), objectc->m_definition(),
    //               objectc->m_child(),  objectc->m_sibling());

    uint64_t field1 = objectc->field1();
    uint64_t field2 = objectc->field2();
    uint64_t field3 = objectc->field3();
    uint64_t field4 = objectc->field4();
    // Decode compression done when saving cache (see below)
    // clang-format off
    RawSymbolId name =      (field1 & 0x0000000000FFFFFF);
    uint16_t type =         (field1 & 0x0000000FFF000000) >> (24);
    uint16_t column =       (field1 & 0x0000FFF000000000) >> (24 + 12);
    RawNodeId parent =      (field1 & 0xFFFF000000000000) >> (24 + 12 + 12);
    parent |=               (field2 & 0x0000000000000FFF) << (16);
    RawNodeId definition =  (field2 & 0x000000FFFFFFF000) >> (12);
    RawNodeId child =       (field2 & 0xFFFFFF0000000000) >> (12 + 28);
    child |=                (field3 & 0x000000000000000F) << (24);
    RawNodeId sibling =     (field3 & 0x00000000FFFFFFF0) >> (4);
    RawSymbolId fileId =    (field3 & 0x00FFFFFF00000000) >> (4 + 28);
    uint32_t line =         (field3 & 0xFF00000000000000) >> (4 + 28 + 24);
    line |=                 (field4 & 0x000000000000FFFF) << (8);
    uint32_t endLine =      (field4 & 0x000000FFFFFF0000) >> (16);
    uint16_t endColumn =    (field4 & 0x000FFF0000000000) >> (16 + 24);
    // clang-format on

    result->emplace_back(
        localSymbols->copyFrom(SymbolId(name, UnknownRawPath), &cacheSymbols),
        fileSystem->toPathId(fileSystem->remap(cacheSymbols.getSymbol(
                                 SymbolId(fileId, UnknownRawPath))),
                             localSymbols),
        (VObjectType)type, line, column, endLine, endColumn, NodeId(parent),
        NodeId(definition), NodeId(child), NodeId(sibling));
  }
}
}  // namespace SURELOG

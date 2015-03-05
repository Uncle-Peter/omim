#include "search_index_builder.hpp"
#include "feature_utils.hpp"
#include "features_vector.hpp"
#include "search_delimiters.hpp"
#include "search_trie.hpp"
#include "search_string_utils.hpp"
#include "string_file.hpp"
#include "string_file_values.hpp"
#include "classificator.hpp"
#include "feature_visibility.hpp"
#include "categories_holder.hpp"
#include "feature_algo.hpp"

#include "../search/search_common.hpp"    // for MAX_TOKENS constant

#include "../defines.hpp"

#include "../platform/platform.hpp"

#include "../coding/trie_builder.hpp"
#include "../coding/writer.hpp"
#include "../coding/reader_writer_ops.hpp"

#include "../base/assert.hpp"
#include "../base/timer.hpp"
#include "../base/scope_guard.hpp"
#include "../base/string_utils.hpp"
#include "../base/logging.hpp"
#include "../base/stl_add.hpp"

#include "../std/algorithm.hpp"
#include "../std/fstream.hpp"
#include "../std/initializer_list.hpp"
#include "../std/limits.hpp"
#include "../std/unordered_map.hpp"
#include "../std/vector.hpp"

#define SYNONYMS_FILE "synonyms.txt"


namespace
{

class SynonymsHolder
{
  unordered_multimap<string, string> m_map;

public:
  SynonymsHolder(string const & fPath)
  {
    ifstream stream(fPath.c_str());

    string line;
    vector<string> tokens;

    while (stream.good())
    {
      std::getline(stream, line);
      if (line.empty())
        continue;

      tokens.clear();
      strings::Tokenize(line, ":,", MakeBackInsertFunctor(tokens));

      if (tokens.size() > 1)
      {
        strings::Trim(tokens[0]);
        for (size_t i = 1; i < tokens.size(); ++i)
        {
          strings::Trim(tokens[i]);
          // synonym should not has any spaces
          ASSERT ( tokens[i].find_first_of(" \t") == string::npos, () );
          m_map.insert(make_pair(tokens[0], tokens[i]));
        }
      }
    }
  }

  template <class ToDo> void ForEach(string const & key, ToDo toDo) const
  {
    typedef unordered_multimap<string, string>::const_iterator IterT;

    pair<IterT, IterT> range = m_map.equal_range(key);
    while (range.first != range.second)
    {
      toDo(range.first->second);
      ++range.first;
    }
  }
};

template<typename StringsFileT>
struct FeatureNameInserter
{
  SynonymsHolder * m_synonyms;
  StringsFileT & m_names;
  typename StringsFileT::ValueT m_val;

  FeatureNameInserter(SynonymsHolder * synonyms, StringsFileT & names)
    : m_synonyms(synonyms), m_names(names)
  {
  }

  void AddToken(signed char lang, strings::UniString const & s) const
  {
    m_names.AddString(typename StringsFileT::StringT(s, lang, m_val));
  }

private:
  typedef buffer_vector<strings::UniString, 32> TokensArrayT;

  class PushSynonyms
  {
    TokensArrayT & m_tokens;
  public:
    PushSynonyms(TokensArrayT & tokens) : m_tokens(tokens) {}
    void operator() (string const & utf8str) const
    {
      m_tokens.push_back(search::NormalizeAndSimplifyString(utf8str));
    }
  };

public:
  bool operator()(signed char lang, string const & name) const
  {
    strings::UniString const uniName = search::NormalizeAndSimplifyString(name);

    // split input string on tokens
    buffer_vector<strings::UniString, 32> tokens;
    SplitUniString(uniName, MakeBackInsertFunctor(tokens), search::Delimiters());

    // add synonyms for input native string
    if (m_synonyms)
      m_synonyms->ForEach(name, PushSynonyms(tokens));

    int const maxTokensCount = search::MAX_TOKENS - 1;
    if (tokens.size() > maxTokensCount)
    {
      LOG(LWARNING, ("Name has too many tokens:", name));
      tokens.resize(maxTokensCount);
    }

    for (size_t i = 0; i < tokens.size(); ++i)
      AddToken(lang, tokens[i]);

    return true;
  }
};

template <typename ValueT>
struct ValueBuilder;

template <>
struct ValueBuilder<SerializedFeatureInfoValue>
{
  typedef search::trie::ValueReader SaverT;
  SaverT m_valueSaver;

  ValueBuilder(serial::CodingParams const & cp) : m_valueSaver(cp) {}

  void MakeValue(FeatureType const & f, feature::TypesHolder const & types, uint64_t pos,
                 SerializedFeatureInfoValue & value) const
  {
    SaverT::ValueType v;
    v.m_featureId = static_cast<uint32_t>(pos);

    // get BEST geometry rect of feature
    v.m_pt = feature::GetCenter(f);
    v.m_rank = feature::GetSearchRank(types, v.m_pt, f.GetPopulation());

    // write to buffer
    PushBackByteSink<SerializedFeatureInfoValue::ValueT> sink(value.m_value);
    m_valueSaver.Save(sink, v);
  }
};

template <>
struct ValueBuilder<FeatureIndexValue>
{
  void MakeValue(FeatureType const & /* f */, feature::TypesHolder const & /* types */,
                 uint64_t pos, FeatureIndexValue & value) const
  {
    ASSERT_LESS(pos, numeric_limits<uint32_t>::max(), ());
    value.m_value = static_cast<uint32_t>(pos);
  }
};

template <typename StringsFileT>
class FeatureInserter
{
  SynonymsHolder * m_synonyms;
  StringsFileT & m_names;

  CategoriesHolder const & m_categories;

  typedef typename StringsFileT::ValueT ValueT;
  typedef search::trie::ValueReader SaverT;

  pair<int, int> m_scales;

  ValueBuilder<ValueT> const & m_valueBuilder;

  /// There are 3 different ways of search index skipping:
  /// - skip features in any case (m_skipFeatures)
  /// - skip features with empty names (m_enFeature)
  /// - skip specified types for features with empty names (m_enTypes)
  class SkipIndexing
  {
    typedef buffer_vector<uint32_t, 16> ContT;

    // Array index (0, 1) means type level for checking (1, 2).
    ContT m_skipEn[2], m_skipF[2];
    ContT m_dontSkipEn;
    uint32_t m_country, m_state;

    static bool HasType(ContT const & v, uint32_t t)
    {
      return (find(v.begin(), v.end(), t) != v.end());
    }

  public:
    SkipIndexing()
    {
      Classificator const & c = classif();

      // Fill types that always! should be skipped.
      for (auto const & e : (StringIL[]) { { "entrance" } })
        m_skipF[0].push_back(c.GetTypeByPath(e));

      for (auto const & e : (StringIL[]) { { "building", "address" } })
        m_skipF[1].push_back(c.GetTypeByPath(e));

      // Fill types that never! will be skipped.
      for (auto const & e : (StringIL[]) { { "highway", "bus_stop" }, { "highway", "speed_camera" } })
        m_dontSkipEn.push_back(c.GetTypeByPath(e));

      // Fill types that will be skipped if feature's name is empty!
      for (auto const & e : (StringIL[]) { { "building" }, { "highway" }, { "natural" }, { "waterway" }, { "landuse" } })
        m_skipEn[0].push_back(c.GetTypeByPath(e));

      for (auto const & e : (StringIL[]) {
        { "place", "country" },
        { "place", "state" },
        { "place", "county" },
        { "place", "region" },
        { "place", "city" },
        { "place", "town" },
        { "railway", "rail" }})
      {
        m_skipEn[1].push_back(c.GetTypeByPath(e));
      }

      m_country = c.GetTypeByPath({ "place", "country" });
      m_state = c.GetTypeByPath({ "place", "state" });
    }

    void SkipTypes(feature::TypesHolder & types) const
    {
      types.RemoveIf([this](uint32_t type)
      {
        ftype::TruncValue(type, 2);

        if (HasType(m_skipF[1], type))
          return true;

        ftype::TruncValue(type, 1);

        if (HasType(m_skipF[0], type))
          return true;

        return false;
      });
    }

    void SkipEmptyNameTypes(feature::TypesHolder & types) const
    {
      types.RemoveIf([this](uint32_t type)
      {
        ftype::TruncValue(type, 2);

        if (HasType(m_dontSkipEn, type))
          return false;

        if (HasType(m_skipEn[1], type))
          return true;

        ftype::TruncValue(type, 1);

        if (HasType(m_skipEn[0], type))
          return true;

        return false;
      });
    }

    bool IsCountryOrState(feature::TypesHolder const & types) const
    {
      for (uint32_t t : types)
      {
        ftype::TruncValue(t, 2);
        if (t == m_country || t == m_state)
          return true;
      }
      return false;
    }
  };

public:
  FeatureInserter(SynonymsHolder * synonyms, StringsFileT & names,
                  CategoriesHolder const & catHolder, pair<int, int> const & scales,
                  ValueBuilder<ValueT> const & valueBuilder)
    : m_synonyms(synonyms), m_names(names),
      m_categories(catHolder), m_scales(scales), m_valueBuilder(valueBuilder)
  {
  }

  void operator() (FeatureType const & f, uint64_t pos) const
  {
    feature::TypesHolder types(f);

    static SkipIndexing skipIndex;

    skipIndex.SkipTypes(types);
    if (types.Empty())
      return;

    // Init inserter with serialized value.
    // Insert synonyms only for countries and states (maybe will add cities in future).
    FeatureNameInserter<StringsFileT> inserter(skipIndex.IsCountryOrState(types) ? m_synonyms : 0,
                                               m_names);
    m_valueBuilder.MakeValue(f, types, pos, inserter.m_val);

    // Skip types for features without names.
    if (!f.ForEachNameRef(inserter))
      skipIndex.SkipEmptyNameTypes(types);
    if (types.Empty())
      return;

    Classificator const & c = classif();

    // add names of categories of the feature
    for (uint32_t t : types)
    {
      // Leave only 2 level of type - for example, do not distinguish:
      // highway-primary-bridge or amenity-parking-fee.
      ftype::TruncValue(t, 2);

      // Push to index only categorized types.
      if (m_categories.IsTypeExist(t))
      {
        // Do index only for visible types in mwm.
        pair<int, int> const r = feature::GetDrawableScaleRange(t);
        CHECK(r.first <= r.second && r.first != -1, (c.GetReadableObjectName(t)));

        if (r.second >= m_scales.first && r.first <= m_scales.second)
        {
          inserter.AddToken(search::CATEGORIES_LANG,
                            search::FeatureTypeToString(c.GetIndexForType(t)));
        }
      }
    }
  }
};

template <typename FeatureInserterT>
struct FeatureInserterAdapter
{
  FeatureInserterAdapter(FeatureInserterT & inserter) : m_inserter(inserter), m_index(0) {}

  void operator()(FeatureType const & f, uint64_t pos)
  {
    /// @todo After VNG's refactoring the whole class should go away
    ///       since pos will be replaced by a feature's index.
    m_inserter(f, m_index++);
  }

  FeatureInserterT & m_inserter;
  uint64_t m_index;
};

void AddFeatureNameIndexPairs(FilesContainerR const & container,
                              CategoriesHolder & categoriesHolder,
                              StringsFile<FeatureIndexValue> & stringsFile)
{
  feature::DataHeader header;
  header.Load(container.GetReader(HEADER_FILE_TAG));
  FeaturesVector features(container, header);

  ValueBuilder<FeatureIndexValue> valueBuilder;

  unique_ptr<SynonymsHolder> synonyms;
  if (header.GetType() == feature::DataHeader::world)
    synonyms.reset(new SynonymsHolder(GetPlatform().WritablePathForFile(SYNONYMS_FILE)));

  FeatureInserter<StringsFile<FeatureIndexValue>> inserter(
      synonyms.get(), stringsFile, categoriesHolder, header.GetScaleRange(), valueBuilder);

  FeatureInserterAdapter<FeatureInserter<StringsFile<FeatureIndexValue>>> adapter(inserter);
  features.ForEachOffset(adapter);
}

void BuildSearchIndex(FilesContainerR const & cont, CategoriesHolder const & catHolder,
                      Writer & writer, string const & tmpFilePath)
{
  {
    feature::DataHeader header;
    header.Load(cont.GetReader(HEADER_FILE_TAG));
    FeaturesVector featuresV(cont, header);

    serial::CodingParams cp(search::GetCPForTrie(header.GetDefCodingParams()));
    ValueBuilder<SerializedFeatureInfoValue> valueBuilder(cp);

    unique_ptr<SynonymsHolder> synonyms;
    if (header.GetType() == feature::DataHeader::world)
      synonyms.reset(new SynonymsHolder(GetPlatform().WritablePathForFile(SYNONYMS_FILE)));

    StringsFile<SerializedFeatureInfoValue> names(tmpFilePath);

    featuresV.ForEachOffset(FeatureInserter<StringsFile<SerializedFeatureInfoValue>>(
        synonyms.get(), names, catHolder, header.GetScaleRange(), valueBuilder));

    names.EndAdding();
    names.OpenForRead();

    trie::Build<Writer, typename StringsFile<SerializedFeatureInfoValue>::IteratorT,
                trie::builder::EmptyEdgeBuilder, ValueList<SerializedFeatureInfoValue>>(
        writer, names.Begin(), names.End(), trie::builder::EmptyEdgeBuilder());

    // at this point all readers of StringsFile should be dead
  }

  FileWriter::DeleteFileX(tmpFilePath);
}
}  // namespace

namespace indexer {
bool BuildSearchIndexFromDatFile(string const & datFile, bool forceRebuild)
{
  LOG(LINFO, ("Start building search index. Bits = ", search::POINT_CODING_BITS));

  try
  {
    Platform & pl = GetPlatform();
    string const tmpFile1 = datFile + ".search_index_1.tmp";
    string const tmpFile2 = datFile + ".search_index_2.tmp";

    {
      FilesContainerR readCont(datFile);

      if (!forceRebuild && readCont.IsExist(SEARCH_INDEX_FILE_TAG))
        return true;

      FileWriter writer(tmpFile2);

      CategoriesHolder catHolder(pl.GetReader(SEARCH_CATEGORIES_FILE_NAME));

      BuildSearchIndex(readCont, catHolder, writer, tmpFile1);

      LOG(LINFO, ("Search index size = ", writer.Size()));
    }

    {
      // Write to container in reversed order.
      FilesContainerW writeCont(datFile, FileWriter::OP_WRITE_EXISTING);
      FileWriter writer = writeCont.GetWriter(SEARCH_INDEX_FILE_TAG);
      rw_ops::Reverse(FileReader(tmpFile2), writer);
    }

    FileWriter::DeleteFileX(tmpFile2);
  }
  catch (Reader::Exception const & e)
  {
    LOG(LERROR, ("Error while reading file: ", e.Msg()));
    return false;
  }
  catch (Writer::Exception const & e)
  {
    LOG(LERROR, ("Error writing index file: ", e.Msg()));
    return false;
  }

  LOG(LINFO, ("End building search index."));
  return true;
}

bool AddCompresedSearchIndexSection(string const & fName, bool forceRebuild)
{
  Platform & platform = GetPlatform();

  FilesContainerR readContainer(platform.GetReader(fName));
  if (readContainer.IsExist(COMPRESSED_SEARCH_INDEX_FILE_TAG) && !forceRebuild)
    return true;

  string const indexFile = platform.WritablePathForFile("compressed-search-index.tmp");
  MY_SCOPE_GUARD(indexFileGuard, bind(&FileWriter::DeleteFileX, indexFile));

  try
  {
    {
      FileWriter indexWriter(indexFile);
      BuildCompressedSearchIndex(readContainer, indexWriter);
    }
    {
      FilesContainerW writeContainer(readContainer.GetFileName(), FileWriter::OP_WRITE_EXISTING);
      FileWriter writer = writeContainer.GetWriter(COMPRESSED_SEARCH_INDEX_FILE_TAG);
      rw_ops::Reverse(FileReader(indexFile), writer);
    }
  }
  catch (Reader::Exception const & e)
  {
    LOG(LERROR, ("Error while reading file: ", e.Msg()));
    return false;
  }
  catch (Writer::Exception const & e)
  {
    LOG(LERROR, ("Error writing index file: ", e.Msg()));
    return false;
  }

  return true;
}

void BuildCompressedSearchIndex(FilesContainerR & container, Writer & indexWriter)
{
  Platform & platform = GetPlatform();

  LOG(LINFO, ("Start building compressed search index for", container.GetFileName()));
  my::Timer timer;

  string stringsFilePath = platform.WritablePathForFile("strings.tmp");
  StringsFile<FeatureIndexValue> stringsFile(stringsFilePath);
  MY_SCOPE_GUARD(stringsFileGuard, bind(&FileWriter::DeleteFileX, stringsFilePath));

  CategoriesHolder categoriesHolder(platform.GetReader(SEARCH_CATEGORIES_FILE_NAME));

  AddFeatureNameIndexPairs(container, categoriesHolder, stringsFile);

  stringsFile.EndAdding();

  LOG(LINFO, ("End sorting strings:", timer.ElapsedSeconds()));

  stringsFile.OpenForRead();
  trie::Build<Writer, typename StringsFile<FeatureIndexValue>::IteratorT,
              trie::builder::EmptyEdgeBuilder, ValueList<FeatureIndexValue>>(
      indexWriter, stringsFile.Begin(), stringsFile.End(), trie::builder::EmptyEdgeBuilder());

  LOG(LINFO, ("End building compressed search index, elapsed seconds:", timer.ElapsedSeconds()));
}

void BuildCompressedSearchIndex(string const & fName, Writer & indexWriter)
{
  FilesContainerR container(GetPlatform().GetReader(fName));
  BuildCompressedSearchIndex(container, indexWriter);
}
}  // namespace indexer

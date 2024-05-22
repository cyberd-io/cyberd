// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>

#include <chainparamsbase.h>
#include <logging.h>
#include <sync.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/settings.h>
#include <util/strencodings.h>

#ifdef WIN32
#include <codecvt>    /* for codecvt_utf8_utf16 */
#include <shellapi.h> /* for CommandLineToArgvW */
#include <shlobj.h>   /* for CSIDL_APPDATA */
#endif

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

const char *const BITCOIN_CONF_FILENAME = "bitcoin.conf";
const char *const BITCOIN_SETTINGS_FILENAME = "settings.json";

ArgsManager gArgs;

/**
 * Interpret a string argument as a boolean.
 *
 * The definition of atoi() requires that non-numeric string values like "foo",
 * return 0. This means that if a user unintentionally supplies a non-integer
 * argument here, the return value is always false. This means that -foo=false
 * does what the user probably expects, but -foo=true is well defined but does
 * not do what they probably expected.
 *
 * The return value of atoi() is undefined when given input not representable as
 * an int. On most systems this means string value between "-2147483648" and
 * "2147483647" are well defined (this method will return true). Setting
 * -txindex=2147483648 on most systems, however, is probably undefined.
 *
 * For a more extensive discussion of this topic (and a wide range of opinions
 * on the Right Way to change this code), see PR12713.
 */
static bool InterpretBool(const std::string &strValue) {
    if (strValue.empty()) {
        return true;
    }
    return (atoi(strValue) != 0);
}

static std::string SettingName(const std::string &arg) {
    return arg.size() > 0 && arg[0] == '-' ? arg.substr(1) : arg;
}

/**
 * Interpret -nofoo as if the user supplied -foo=0.
 *
 * This method also tracks when the -no form was supplied, and if so, checks
 * whether there was a double-negative (-nofoo=0 -> -foo=1).
 *
 * If there was not a double negative, it removes the "no" from the key
 * and returns false.
 *
 * If there was a double negative, it removes "no" from the key, and
 * returns true.
 *
 * If there was no "no", it returns the string value untouched.
 *
 * Where an option was negated can be later checked using the IsArgNegated()
 * method. One use case for this is to have a way to disable options that are
 * not normally boolean (e.g. using -nodebuglogfile to request that debug log
 * output is not sent to any file at all).
 */
util::SettingsValue InterpretOption(std::string &section, std::string &key,
                                    const std::string &value) {
    // Split section name from key name for keys like "testnet.foo" or
    // "regtest.bar"
    size_t option_index = key.find('.');
    if (option_index != std::string::npos) {
        section = key.substr(0, option_index);
        key.erase(0, option_index + 1);
    }
    if (key.substr(0, 2) == "no") {
        key.erase(0, 2);
        // Double negatives like -nofoo=0 are supported (but discouraged)
        if (!InterpretBool(value)) {
            LogPrintf("Warning: parsed potentially confusing double-negative "
                      "-%s=%s\n",
                      key, value);
            return true;
        }
        return false;
    }
    return value;
}

/**
 * Check settings value validity according to flags.
 *
 * TODO: Add more meaningful error checks here in the future
 * See "here's how the flags are meant to behave" in
 * https://github.com/bitcoin/bitcoin/pull/16097#issuecomment-514627823
 */
bool CheckValid(const std::string &key, const util::SettingsValue &val,
                unsigned int flags, std::string &error) {
    if (val.isBool() && !(flags & ArgsManager::ALLOW_BOOL)) {
        error = strprintf(
            "Negating of -%s is meaningless and therefore forbidden", key);
        return false;
    }
    return true;
}

// Define default constructor and destructor that are not inline, so code
// instantiating this class doesn't need to #include class definitions for all
// members. For example, m_settings has an internal dependency on univalue.
ArgsManager::ArgsManager() {}
ArgsManager::~ArgsManager() {}

const std::set<std::string> ArgsManager::GetUnsuitableSectionOnlyArgs() const {
    std::set<std::string> unsuitables;

    LOCK(cs_args);

    // if there's no section selected, don't worry
    if (m_network.empty()) {
        return std::set<std::string>{};
    }

    // if it's okay to use the default section for this network, don't worry
    if (m_network == CBaseChainParams::MAIN) {
        return std::set<std::string>{};
    }

    for (const auto &arg : m_network_only_args) {
        if (OnlyHasDefaultSectionSetting(m_settings, m_network,
                                         SettingName(arg))) {
            unsuitables.insert(arg);
        }
    }
    return unsuitables;
}

const std::list<SectionInfo> ArgsManager::GetUnrecognizedSections() const {
    // Section names to be recognized in the config file.
    static const std::set<std::string> available_sections{
        CBaseChainParams::REGTEST, CBaseChainParams::TESTNET,
        CBaseChainParams::MAIN};

    LOCK(cs_args);
    std::list<SectionInfo> unrecognized = m_config_sections;
    unrecognized.remove_if([](const SectionInfo &appeared) {
        return available_sections.find(appeared.m_name) !=
               available_sections.end();
    });
    return unrecognized;
}

void ArgsManager::SelectConfigNetwork(const std::string &network) {
    LOCK(cs_args);
    m_network = network;
}

bool ParseKeyValue(std::string &key, std::string &val) {
    size_t is_index = key.find('=');
    if (is_index != std::string::npos) {
        val = key.substr(is_index + 1);
        key.erase(is_index);
    }
#ifdef WIN32
    key = ToLower(key);
    if (key[0] == '/') {
        key[0] = '-';
    }
#endif

    if (key[0] != '-') {
        return false;
    }

    // Transform --foo to -foo
    if (key.length() > 1 && key[1] == '-') {
        key.erase(0, 1);
    }
    return true;
}

bool ArgsManager::ParseParameters(int argc, const char *const argv[],
                                  std::string &error) {
    LOCK(cs_args);
    m_settings.command_line_options.clear();

    for (int i = 1; i < argc; i++) {
        std::string key(argv[i]);

#ifdef MAC_OSX
        // At the first time when a user gets the "App downloaded from the
        // internet" warning, and clicks the Open button, macOS passes
        // a unique process serial number (PSN) as -psn_... command-line
        // argument, which we filter out.
        if (key.substr(0, 5) == "-psn_") {
            continue;
        }
#endif

        if (key == "-") {
            // bitcoin-tx using stdin
            break;
        }
        std::string val;
        if (!ParseKeyValue(key, val)) {
            break;
        }

        // Transform -foo to foo
        key.erase(0, 1);
        std::string section;
        util::SettingsValue value = InterpretOption(section, key, val);
        std::optional<unsigned int> flags = GetArgFlags('-' + key);

        // Unknown command line options and command line options with dot
        // characters (which are returned from InterpretOption with nonempty
        // section strings) are not valid.
        if (!flags || !section.empty()) {
            error = strprintf("Invalid parameter %s", argv[i]);
            return false;
        }

        if (!CheckValid(key, value, *flags, error)) {
            return false;
        }

        m_settings.command_line_options[key].push_back(value);
    }

    // we do not allow -includeconf from command line
    bool success = true;
    if (auto *includes =
            util::FindKey(m_settings.command_line_options, "includeconf")) {
        for (const auto &include : util::SettingsSpan(*includes)) {
            error +=
                "-includeconf cannot be used from commandline; -includeconf=" +
                include.get_str() + "\n";
            success = false;
        }
    }
    return success;
}

std::optional<unsigned int>
ArgsManager::GetArgFlags(const std::string &name) const {
    LOCK(cs_args);
    for (const auto &arg_map : m_available_args) {
        const auto search = arg_map.second.find(name);
        if (search != arg_map.second.end()) {
            return search->second.m_flags;
        }
    }
    return std::nullopt;
}

fs::path ArgsManager::GetPathArg(std::string arg,
                                 const fs::path &default_value) const {
    if (IsArgNegated(arg)) {
        return fs::path{};
    }
    std::string path_str = GetArg(arg, "");
    if (path_str.empty()) {
        return default_value;
    }
    fs::path result = fs::PathFromString(path_str).lexically_normal();
    // Remove trailing slash, if present.
    return result.has_filename() ? result : result.parent_path();
}

const fs::path &ArgsManager::GetBlocksDirPath() const {
    LOCK(cs_args);
    fs::path &path = m_cached_blocks_path;

    // Cache the path to avoid calling fs::create_directories on every call of
    // this function
    if (!path.empty()) {
        return path;
    }

    if (IsArgSet("-blocksdir")) {
        path = fs::absolute(GetPathArg("-blocksdir"));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDataDirBase();
    }

    path /= fs::PathFromString(BaseParams().DataDir());
    path /= "blocks";
    fs::create_directories(path);
    return path;
}

const fs::path &ArgsManager::GetDataDir(bool net_specific) const {
    LOCK(cs_args);
    fs::path &path =
        net_specific ? m_cached_network_datadir_path : m_cached_datadir_path;

    // Used cached path if available
    if (!path.empty()) {
        return path;
    }

    const fs::path datadir{GetPathArg("-datadir")};
    if (!datadir.empty()) {
        path = fs::absolute(datadir);
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }

    if (net_specific && !BaseParams().DataDir().empty()) {
        path /= fs::PathFromString(BaseParams().DataDir());
    }

    return path;
}

void ArgsManager::EnsureDataDir() const {
    /**
     * "/wallets" subdirectories are created in all **new**
     * datadirs, because wallet code will create new wallets in the "wallets"
     * subdirectory only if exists already, otherwise it will create them in
     * the top-level datadir where they could interfere with other files.
     * Wallet init code currently avoids creating "wallets" directories itself
     * for backwards compatibility, but this be changed in the future and
     * wallet code here could go away.
     */
    auto path{GetDataDir(/*net_specific=*/false)};
    if (!fs::exists(path)) {
        fs::create_directories(path / "wallets");
    }
    path = GetDataDir(/*net_specific=*/true);
    if (!fs::exists(path)) {
        fs::create_directories(path / "wallets");
    }
}

void ArgsManager::ClearPathCache() {
    LOCK(cs_args);

    m_cached_datadir_path = fs::path();
    m_cached_network_datadir_path = fs::path();
    m_cached_blocks_path = fs::path();
}

std::vector<std::string> ArgsManager::GetArgs(const std::string &strArg) const {
    std::vector<std::string> result;
    for (const util::SettingsValue &value : GetSettingsList(strArg)) {
        result.push_back(value.isFalse()  ? "0"
                         : value.isTrue() ? "1"
                                          : value.get_str());
    }
    return result;
}

bool ArgsManager::IsArgSet(const std::string &strArg) const {
    return !GetSetting(strArg).isNull();
}

bool ArgsManager::InitSettings(std::string &error) {
    EnsureDataDir();
    if (!GetSettingsPath()) {
        return true; // Do nothing if settings file disabled.
    }

    std::vector<std::string> errors;
    if (!ReadSettingsFile(&errors)) {
        error = strprintf("Failed loading settings file:\n- %s\n",
                          Join(errors, "\n- "));
        return false;
    }
    if (!WriteSettingsFile(&errors)) {
        error = strprintf("Failed saving settings file:\n- %s\n",
                          Join(errors, "\n- "));
        return false;
    }
    return true;
}

bool ArgsManager::GetSettingsPath(fs::path *filepath, bool temp,
                                  bool backup) const {
    fs::path settings = GetPathArg("-settings", BITCOIN_SETTINGS_FILENAME);
    if (settings.empty()) {
        return false;
    }
    if (backup) {
        settings += ".bak";
    }
    if (filepath) {
        *filepath = fsbridge::AbsPathJoin(GetDataDirNet(),
                                          temp ? settings + ".tmp" : settings);
    }
    return true;
}

static void SaveErrors(const std::vector<std::string> errors,
                       std::vector<std::string> *error_out) {
    for (const auto &error : errors) {
        if (error_out) {
            error_out->emplace_back(error);
        } else {
            LogPrintf("%s\n", error);
        }
    }
}

bool ArgsManager::ReadSettingsFile(std::vector<std::string> *errors) {
    fs::path path;
    if (!GetSettingsPath(&path, /* temp= */ false)) {
        return true; // Do nothing if settings file disabled.
    }

    LOCK(cs_args);
    m_settings.rw_settings.clear();
    std::vector<std::string> read_errors;
    if (!util::ReadSettings(path, m_settings.rw_settings, read_errors)) {
        SaveErrors(read_errors, errors);
        return false;
    }
    for (const auto &setting : m_settings.rw_settings) {
        std::string section;
        std::string key = setting.first;
        // Split setting key into section and argname
        (void)InterpretOption(section, key, /* value */ {});
        if (!GetArgFlags('-' + key)) {
            LogPrintf("Ignoring unknown rw_settings value %s\n", setting.first);
        }
    }
    return true;
}

bool ArgsManager::WriteSettingsFile(std::vector<std::string> *errors,
                                    bool backup) const {
    fs::path path, path_tmp;
    if (!GetSettingsPath(&path, /*temp=*/false, backup) ||
        !GetSettingsPath(&path_tmp, /*temp=*/true, backup)) {
        throw std::logic_error("Attempt to write settings file when dynamic "
                               "settings are disabled.");
    }

    LOCK(cs_args);
    std::vector<std::string> write_errors;
    if (!util::WriteSettings(path_tmp, m_settings.rw_settings, write_errors)) {
        SaveErrors(write_errors, errors);
        return false;
    }
    if (!RenameOver(path_tmp, path)) {
        SaveErrors(
            {strprintf("Failed renaming settings file %s to %s\n",
                       fs::PathToString(path_tmp), fs::PathToString(path))},
            errors);
        return false;
    }
    return true;
}

util::SettingsValue
ArgsManager::GetPersistentSetting(const std::string &name) const {
    LOCK(cs_args);
    return util::GetSetting(
        m_settings, m_network, name, !UseDefaultSection("-" + name),
        /*ignore_nonpersistent=*/true, /*get_chain_name=*/false);
}

bool ArgsManager::IsArgNegated(const std::string &strArg) const {
    return GetSetting(strArg).isFalse();
}

std::string ArgsManager::GetArg(const std::string &strArg,
                                const std::string &strDefault) const {
    return GetArg(strArg).value_or(strDefault);
}

std::optional<std::string>
ArgsManager::GetArg(const std::string &strArg) const {
    const util::SettingsValue value = GetSetting(strArg);
    return SettingToString(value);
}

std::optional<std::string> SettingToString(const util::SettingsValue &value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (value.isFalse()) {
        return "0";
    }
    if (value.isTrue()) {
        return "1";
    }
    if (value.isNum()) {
        return value.getValStr();
    }
    return value.get_str();
}

std::string SettingToString(const util::SettingsValue &value,
                            const std::string &strDefault) {
    return SettingToString(value).value_or(strDefault);
}

int64_t ArgsManager::GetIntArg(const std::string &strArg,
                               int64_t nDefault) const {
    return GetIntArg(strArg).value_or(nDefault);
}

std::optional<int64_t> ArgsManager::GetIntArg(const std::string &strArg) const {
    const util::SettingsValue value = GetSetting(strArg);
    return SettingToInt(value);
}

std::optional<int64_t> SettingToInt(const util::SettingsValue &value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (value.isFalse()) {
        return 0;
    }
    if (value.isTrue()) {
        return 1;
    }
    if (value.isNum()) {
        return value.get_int64();
    }
    return atoi64(value.get_str());
}

int64_t SettingToInt(const util::SettingsValue &value, int64_t nDefault) {
    return SettingToInt(value).value_or(nDefault);
}

bool ArgsManager::GetBoolArg(const std::string &strArg, bool fDefault) const {
    return GetBoolArg(strArg).value_or(fDefault);
}

std::optional<bool> ArgsManager::GetBoolArg(const std::string &strArg) const {
    const util::SettingsValue value = GetSetting(strArg);
    return SettingToBool(value);
}

std::optional<bool> SettingToBool(const util::SettingsValue &value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (value.isBool()) {
        return value.get_bool();
    }
    return InterpretBool(value.get_str());
}

bool SettingToBool(const util::SettingsValue &value, bool fDefault) {
    return SettingToBool(value).value_or(fDefault);
}

bool ArgsManager::SoftSetArg(const std::string &strArg,
                             const std::string &strValue) {
    LOCK(cs_args);
    if (IsArgSet(strArg)) {
        return false;
    }
    ForceSetArg(strArg, strValue);
    return true;
}

bool ArgsManager::SoftSetBoolArg(const std::string &strArg, bool fValue) {
    if (fValue) {
        return SoftSetArg(strArg, std::string("1"));
    } else {
        return SoftSetArg(strArg, std::string("0"));
    }
}

void ArgsManager::ForceSetArg(const std::string &strArg,
                              const std::string &strValue) {
    LOCK(cs_args);
    m_settings.forced_settings[SettingName(strArg)] = strValue;
}

/**
 * This function is only used for testing purpose so
 * so we should not worry about element uniqueness and
 * integrity of the data structure
 */
void ArgsManager::ForceSetMultiArg(const std::string &strArg,
                                   const std::vector<std::string> &values) {
    LOCK(cs_args);
    util::SettingsValue value;
    value.setArray();
    for (const std::string &s : values) {
        value.push_back(s);
    }

    m_settings.forced_settings[SettingName(strArg)] = value;
}

void ArgsManager::AddArg(const std::string &name, const std::string &help,
                         unsigned int flags, const OptionsCategory &cat) {
    // Split arg name from its help param
    size_t eq_index = name.find('=');
    if (eq_index == std::string::npos) {
        eq_index = name.size();
    }
    std::string arg_name = name.substr(0, eq_index);

    LOCK(cs_args);
    std::map<std::string, Arg> &arg_map = m_available_args[cat];
    auto ret = arg_map.emplace(
        arg_name,
        Arg{name.substr(eq_index, name.size() - eq_index), help, flags});
    // Make sure an insertion actually happened.
    assert(ret.second);

    if (flags & ArgsManager::NETWORK_ONLY) {
        m_network_only_args.emplace(arg_name);
    }
}

void ArgsManager::AddHiddenArgs(const std::vector<std::string> &names) {
    for (const std::string &name : names) {
        AddArg(name, "", ArgsManager::ALLOW_ANY, OptionsCategory::HIDDEN);
    }
}

void ArgsManager::ClearForcedArg(const std::string &strArg) {
    LOCK(cs_args);
    m_settings.forced_settings.erase(SettingName(strArg));
}

std::string ArgsManager::GetHelpMessage() const {
    const bool show_debug = GetBoolArg("-help-debug", false);

    std::string usage = "";
    LOCK(cs_args);
    for (const auto &arg_map : m_available_args) {
        switch (arg_map.first) {
            case OptionsCategory::OPTIONS:
                usage += HelpMessageGroup("Options:");
                break;
            case OptionsCategory::CONNECTION:
                usage += HelpMessageGroup("Connection options:");
                break;
            case OptionsCategory::ZMQ:
                usage += HelpMessageGroup("ZeroMQ notification options:");
                break;
            case OptionsCategory::DEBUG_TEST:
                usage += HelpMessageGroup("Debugging/Testing options:");
                break;
            case OptionsCategory::NODE_RELAY:
                usage += HelpMessageGroup("Node relay options:");
                break;
            case OptionsCategory::BLOCK_CREATION:
                usage += HelpMessageGroup("Block creation options:");
                break;
            case OptionsCategory::RPC:
                usage += HelpMessageGroup("RPC server options:");
                break;
            case OptionsCategory::WALLET:
                usage += HelpMessageGroup("Wallet options:");
                break;
            case OptionsCategory::WALLET_DEBUG_TEST:
                if (show_debug) {
                    usage +=
                        HelpMessageGroup("Wallet debugging/testing options:");
                }
                break;
            case OptionsCategory::CHAINPARAMS:
                usage += HelpMessageGroup("Chain selection options:");
                break;
            case OptionsCategory::GUI:
                usage += HelpMessageGroup("UI Options:");
                break;
            case OptionsCategory::COMMANDS:
                usage += HelpMessageGroup("Commands:");
                break;
            case OptionsCategory::REGISTER_COMMANDS:
                usage += HelpMessageGroup("Register Commands:");
                break;
            case OptionsCategory::AVALANCHE:
                usage += HelpMessageGroup("Avalanche options:");
                break;
            case OptionsCategory::CHRONIK:
                usage += HelpMessageGroup("Chronik options:");
                break;
            default:
                break;
        }

        // When we get to the hidden options, stop
        if (arg_map.first == OptionsCategory::HIDDEN) {
            break;
        }

        for (const auto &arg : arg_map.second) {
            if (show_debug || !(arg.second.m_flags & ArgsManager::DEBUG_ONLY)) {
                std::string name;
                if (arg.second.m_help_param.empty()) {
                    name = arg.first;
                } else {
                    name = arg.first + arg.second.m_help_param;
                }
                usage += HelpMessageOpt(name, arg.second.m_help_text);
            }
        }
    }
    return usage;
}

bool HelpRequested(const ArgsManager &args) {
    return args.IsArgSet("-?") || args.IsArgSet("-h") ||
           args.IsArgSet("-help") || args.IsArgSet("-help-debug");
}

void SetupHelpOptions(ArgsManager &args) {
    args.AddArg("-?", "Print this help message and exit", false,
                OptionsCategory::OPTIONS);
    args.AddHiddenArgs({"-h", "-help"});
}

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message) {
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option,
                           const std::string &message) {
    return std::string(optIndent, ' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent, ' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

fs::path GetDefaultDataDir() {
    // Windows: C:\Users\Username\AppData\Roaming\Bitcoin
    // macOS: ~/Library/Application Support/Bitcoin
    // Unix-like: ~/.bitcoin
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "Bitcoin";
#else
    fs::path pathRet;
    char *pszHome = getenv("HOME");
    if (pszHome == nullptr || strlen(pszHome) == 0) {
        pathRet = fs::path("/");
    } else {
        pathRet = fs::path(pszHome);
    }
#ifdef MAC_OSX
    // macOS
    return pathRet / "Library/Application Support/Bitcoin";
#else
    // Unix-like
    return pathRet / ".bitcoin";
#endif
#endif
}

bool CheckDataDirOption(const ArgsManager &args) {
    const fs::path datadir{args.GetPathArg("-datadir")};
    return datadir.empty() || fs::is_directory(fs::absolute(datadir));
}

fs::path ArgsManager::GetConfigFilePath() const {
    return GetConfigFile(*this, GetPathArg("-conf", BITCOIN_CONF_FILENAME));
}

std::string ArgsManager::GetChainName() const {
    auto get_net = [&](const std::string &arg) {
        LOCK(cs_args);
        util::SettingsValue value =
            util::GetSetting(m_settings, /*section=*/"", SettingName(arg),
                             /*ignore_default_section_config=*/false,
                             /*ignore_nonpersistent=*/false,
                             /*get_chain_name=*/true);
        return value.isNull()   ? false
               : value.isBool() ? value.get_bool()
                                : InterpretBool(value.get_str());
    };

    const bool fRegTest = get_net("-regtest");
    const bool fTestNet = get_net("-testnet");
    const bool is_chain_arg_set = IsArgSet("-chain");

    if (int(is_chain_arg_set) + int(fRegTest) + int(fTestNet) > 1) {
        throw std::runtime_error("Invalid combination of -regtest, -testnet "
                                 "and -chain. Can use at most one.");
    }
    if (fRegTest) {
        return CBaseChainParams::REGTEST;
    }
    if (fTestNet) {
        return CBaseChainParams::TESTNET;
    }
    return GetArg("-chain", CBaseChainParams::MAIN);
}

bool ArgsManager::UseDefaultSection(const std::string &arg) const {
    return m_network == CBaseChainParams::MAIN ||
           m_network_only_args.count(arg) == 0;
}

util::SettingsValue ArgsManager::GetSetting(const std::string &arg) const {
    LOCK(cs_args);
    return util::GetSetting(m_settings, m_network, SettingName(arg),
                            !UseDefaultSection(arg),
                            /*ignore_nonpersistent=*/false,
                            /*get_chain_name=*/false);
}

std::vector<util::SettingsValue>
ArgsManager::GetSettingsList(const std::string &arg) const {
    LOCK(cs_args);
    return util::GetSettingsList(m_settings, m_network, SettingName(arg),
                                 !UseDefaultSection(arg));
}

void ArgsManager::logArgsPrefix(
    const std::string &prefix, const std::string &section,
    const std::map<std::string, std::vector<util::SettingsValue>> &args) const {
    std::string section_str = section.empty() ? "" : "[" + section + "] ";
    for (const auto &arg : args) {
        for (const auto &value : arg.second) {
            std::optional<unsigned int> flags = GetArgFlags('-' + arg.first);
            if (flags) {
                std::string value_str =
                    (*flags & SENSITIVE) ? "****" : value.write();
                LogPrintf("%s %s%s=%s\n", prefix, section_str, arg.first,
                          value_str);
            }
        }
    }
}

void ArgsManager::LogArgs() const {
    LOCK(cs_args);
    for (const auto &section : m_settings.ro_config) {
        logArgsPrefix("Config file arg:", section.first, section.second);
    }
    for (const auto &setting : m_settings.rw_settings) {
        LogPrintf("Setting file arg: %s = %s\n", setting.first,
                  setting.second.write());
    }
    logArgsPrefix("Command-line arg:", "", m_settings.command_line_options);
}

namespace common {
#ifdef WIN32
WinCmdLineArgs::WinCmdLineArgs() {
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> utf8_cvt;
    argv = new char *[argc];
    args.resize(argc);
    for (int i = 0; i < argc; i++) {
        args[i] = utf8_cvt.to_bytes(wargv[i]);
        argv[i] = &*args[i].begin();
    }
    LocalFree(wargv);
}

WinCmdLineArgs::~WinCmdLineArgs() {
    delete[] argv;
}

std::pair<int, char **> WinCmdLineArgs::get() {
    return std::make_pair(argc, argv);
}
#endif
} // namespace common

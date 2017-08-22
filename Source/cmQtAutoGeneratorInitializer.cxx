/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmQtAutoGeneratorInitializer.h"
#include "cmQtAutoGeneratorCommon.h"

#include "cmAlgorithms.h"
#include "cmCustomCommandLines.h"
#include "cmFilePathChecksum.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmOutputConverter.h"
#include "cmPolicies.h"
#include "cmSourceFile.h"
#include "cmSourceGroup.h"
#include "cmState.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cm_sys_stat.h"
#include "cmake.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#include "cmGlobalVisualStudioGenerator.h"
#endif

#include "cmConfigure.h"
#include "cmsys/FStream.hxx"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

inline static const char* SafeString(const char* value)
{
  return (value != CM_NULLPTR) ? value : "";
}

static std::string GetSafeProperty(cmGeneratorTarget const* target,
                                   const char* key)
{
  return std::string(SafeString(target->GetProperty(key)));
}

inline static bool AutogenMultiConfig(cmGlobalGenerator* globalGen)
{
  return globalGen->IsMultiConfig();
}

static std::string GetAutogenTargetName(cmGeneratorTarget const* target)
{
  std::string autogenTargetName = target->GetName();
  autogenTargetName += "_autogen";
  return autogenTargetName;
}

static std::string GetAutogenTargetFilesDir(cmGeneratorTarget const* target)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  std::string targetDir = makefile->GetCurrentBinaryDirectory();
  targetDir += makefile->GetCMakeInstance()->GetCMakeFilesDirectory();
  targetDir += "/";
  targetDir += GetAutogenTargetName(target);
  targetDir += ".dir";
  return targetDir;
}

static std::string GetAutogenTargetBuildDir(cmGeneratorTarget const* target)
{
  std::string targetDir = GetSafeProperty(target, "AUTOGEN_BUILD_DIR");
  if (targetDir.empty()) {
    cmMakefile* makefile = target->Target->GetMakefile();
    targetDir = makefile->GetCurrentBinaryDirectory();
    targetDir += "/";
    targetDir += GetAutogenTargetName(target);
  }
  return targetDir;
}

static std::string GetQtMajorVersion(cmGeneratorTarget const* target)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  std::string qtMajorVersion = makefile->GetSafeDefinition("QT_VERSION_MAJOR");
  if (qtMajorVersion.empty()) {
    qtMajorVersion = makefile->GetSafeDefinition("Qt5Core_VERSION_MAJOR");
  }
  const char* targetQtVersion =
    target->GetLinkInterfaceDependentStringProperty("QT_MAJOR_VERSION", "");
  if (targetQtVersion != CM_NULLPTR) {
    qtMajorVersion = targetQtVersion;
  }
  return qtMajorVersion;
}

static std::string GetQtMinorVersion(cmGeneratorTarget const* target,
                                     const std::string& qtMajorVersion)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  std::string qtMinorVersion;
  if (qtMajorVersion == "5") {
    qtMinorVersion = makefile->GetSafeDefinition("Qt5Core_VERSION_MINOR");
  }
  if (qtMinorVersion.empty()) {
    qtMinorVersion = makefile->GetSafeDefinition("QT_VERSION_MINOR");
  }

  const char* targetQtVersion =
    target->GetLinkInterfaceDependentStringProperty("QT_MINOR_VERSION", "");
  if (targetQtVersion != CM_NULLPTR) {
    qtMinorVersion = targetQtVersion;
  }
  return qtMinorVersion;
}

static bool QtVersionGreaterOrEqual(const std::string& major,
                                    const std::string& minor,
                                    unsigned long requestMajor,
                                    unsigned long requestMinor)
{
  unsigned long majorUL(0);
  unsigned long minorUL(0);
  if (cmSystemTools::StringToULong(major.c_str(), &majorUL) &&
      cmSystemTools::StringToULong(minor.c_str(), &minorUL)) {
    return (majorUL > requestMajor) ||
      (majorUL == requestMajor && minorUL >= requestMinor);
  }
  return false;
}

static void GetCompileDefinitionsAndDirectories(
  cmGeneratorTarget const* target, const std::string& config,
  std::string& incs, std::string& defs)
{
  cmLocalGenerator* localGen = target->GetLocalGenerator();
  {
    std::vector<std::string> includeDirs;
    // Get the include dirs for this target, without stripping the implicit
    // include dirs off, see
    // https://gitlab.kitware.com/cmake/cmake/issues/13667
    localGen->GetIncludeDirectories(includeDirs, target, "CXX", config, false);
    incs = cmJoin(includeDirs, ";");
  }
  {
    std::set<std::string> defines;
    localGen->AddCompileDefinitions(defines, target, config, "CXX");
    defs += cmJoin(defines, ";");
  }
}

static std::vector<std::string> GetConfigurations(
  cmMakefile* makefile, std::string* config = CM_NULLPTR)
{
  std::vector<std::string> configs;
  {
    std::string cfg = makefile->GetConfigurations(configs);
    if (config != CM_NULLPTR) {
      *config = cfg;
    }
  }
  // Add empty configuration on demand
  if (configs.empty()) {
    configs.push_back("");
  }
  return configs;
}

static std::vector<std::string> GetConfigurationSuffixes(cmMakefile* makefile)
{
  std::vector<std::string> suffixes;
  if (AutogenMultiConfig(makefile->GetGlobalGenerator())) {
    makefile->GetConfigurations(suffixes);
    for (std::vector<std::string>::iterator it = suffixes.begin();
         it != suffixes.end(); ++it) {
      it->insert(0, "_");
    }
  }
  if (suffixes.empty()) {
    suffixes.push_back("");
  }
  return suffixes;
}

static void AddDefinitionEscaped(cmMakefile* makefile, const char* key,
                                 const std::string& value)
{
  makefile->AddDefinition(key,
                          cmOutputConverter::EscapeForCMake(value).c_str());
}

static void AddDefinitionEscaped(cmMakefile* makefile, const char* key,
                                 const std::vector<std::string>& values)
{
  makefile->AddDefinition(
    key, cmOutputConverter::EscapeForCMake(cmJoin(values, ";")).c_str());
}

static bool AddToSourceGroup(cmMakefile* makefile, const std::string& fileName,
                             cmQtAutoGeneratorCommon::GeneratorType genType)
{
  cmSourceGroup* sourceGroup = CM_NULLPTR;
  // Acquire source group
  {
    const char* groupName = CM_NULLPTR;
    // Use generator specific group name
    switch (genType) {
      case cmQtAutoGeneratorCommon::MOC:
        groupName =
          makefile->GetState()->GetGlobalProperty("AUTOMOC_SOURCE_GROUP");
        break;
      case cmQtAutoGeneratorCommon::RCC:
        groupName =
          makefile->GetState()->GetGlobalProperty("AUTORCC_SOURCE_GROUP");
        break;
      default:
        break;
    }
    // Use default group name on demand
    if ((groupName == CM_NULLPTR) || (*groupName == 0)) {
      groupName =
        makefile->GetState()->GetGlobalProperty("AUTOGEN_SOURCE_GROUP");
    }
    // Generate a source group on demand
    if ((groupName != CM_NULLPTR) && (*groupName != 0)) {
      {
        const char* delimiter =
          makefile->GetDefinition("SOURCE_GROUP_DELIMITER");
        if (delimiter == CM_NULLPTR) {
          delimiter = "\\";
        }
        std::vector<std::string> folders =
          cmSystemTools::tokenize(groupName, delimiter);
        sourceGroup = makefile->GetSourceGroup(folders);
        if (sourceGroup == CM_NULLPTR) {
          makefile->AddSourceGroup(folders);
          sourceGroup = makefile->GetSourceGroup(folders);
        }
      }
      if (sourceGroup == CM_NULLPTR) {
        cmSystemTools::Error(
          "Autogen: Could not create or find source group: ",
          cmQtAutoGeneratorCommon::Quoted(groupName).c_str());
        return false;
      }
    }
  }
  if (sourceGroup != CM_NULLPTR) {
    sourceGroup->AddGroupFile(fileName);
  }
  return true;
}

static void AddCleanFile(cmMakefile* makefile, const std::string& fileName)
{
  makefile->AppendProperty("ADDITIONAL_MAKE_CLEAN_FILES", fileName.c_str(),
                           false);
}

static void AddGeneratedSource(cmGeneratorTarget* target,
                               const std::string& filename,
                               cmQtAutoGeneratorCommon::GeneratorType genType)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  {
    cmSourceFile* gFile = makefile->GetOrCreateSource(filename, true);
    gFile->SetProperty("GENERATED", "1");
    gFile->SetProperty("SKIP_AUTOGEN", "On");
  }
  target->AddSource(filename);

  AddToSourceGroup(makefile, filename, genType);
}

struct AutogenSetup
{
  std::vector<std::string> sources;
  std::vector<std::string> headers;

  std::vector<std::string> mocSkip;
  std::vector<std::string> uicSkip;

  std::map<std::string, std::string> configSuffix;
  std::map<std::string, std::string> configMocIncludes;
  std::map<std::string, std::string> configMocDefines;
  std::map<std::string, std::string> configUicOptions;
};

static void SetupAcquireScanFiles(cmGeneratorTarget const* target,
                                  bool mocEnabled, bool uicEnabled,
                                  const std::vector<cmSourceFile*>& srcFiles,
                                  AutogenSetup& setup)
{
  const cmPolicies::PolicyStatus CMP0071_status =
    target->Makefile->GetPolicyStatus(cmPolicies::CMP0071);

  for (std::vector<cmSourceFile*>::const_iterator fileIt = srcFiles.begin();
       fileIt != srcFiles.end(); ++fileIt) {
    cmSourceFile* sf = *fileIt;
    // sf->GetExtension() is only valid after sf->GetFullPath() ...
    const std::string& fPath = sf->GetFullPath();
    const cmSystemTools::FileFormat fileType =
      cmSystemTools::GetFileFormat(sf->GetExtension().c_str());
    if (!(fileType == cmSystemTools::CXX_FILE_FORMAT) &&
        !(fileType == cmSystemTools::HEADER_FILE_FORMAT)) {
      continue;
    }
    // Real file path
    const std::string absFile = cmsys::SystemTools::GetRealPath(fPath);
    // Skip flags
    const bool skipAll = sf->GetPropertyAsBool("SKIP_AUTOGEN");
    const bool mocSkip = skipAll || sf->GetPropertyAsBool("SKIP_AUTOMOC");
    const bool uicSkip = skipAll || sf->GetPropertyAsBool("SKIP_AUTOUIC");
    const bool accept = (mocEnabled && !mocSkip) || (uicEnabled && !uicSkip);

    // For GENERATED files check status of policy CMP0071
    if (accept && sf->GetPropertyAsBool("GENERATED")) {
      bool policyAccept = false;
      switch (CMP0071_status) {
        case cmPolicies::WARN: {
          std::ostringstream ost;
          ost << cmPolicies::GetPolicyWarning(cmPolicies::CMP0071) << "\n";
          ost << "AUTOMOC/AUTOUIC: Ignoring GENERATED source file:\n";
          ost << "  " << cmQtAutoGeneratorCommon::Quoted(absFile) << "\n";
          target->Makefile->IssueMessage(cmake::AUTHOR_WARNING, ost.str());
        }
          CM_FALLTHROUGH;
        case cmPolicies::OLD:
          // Ignore GENERATED file
          break;
        case cmPolicies::REQUIRED_IF_USED:
        case cmPolicies::REQUIRED_ALWAYS:
        case cmPolicies::NEW:
          // Process GENERATED file
          policyAccept = true;
          break;
      }
      if (!policyAccept) {
        continue;
      }
    }

    // Add file name to skip lists.
    // Do this even when the file is not added to the sources/headers lists
    // because the file name may be extracted from an other file when
    // processing
    if (mocSkip) {
      setup.mocSkip.push_back(absFile);
    }
    if (uicSkip) {
      setup.uicSkip.push_back(absFile);
    }

    if (accept) {
      // Add file name to sources or headers list
      switch (fileType) {
        case cmSystemTools::CXX_FILE_FORMAT:
          setup.sources.push_back(absFile);
          break;
        case cmSystemTools::HEADER_FILE_FORMAT:
          setup.headers.push_back(absFile);
          break;
        default:
          break;
      }
    }
  }
}

static void SetupAutoTargetMoc(cmGeneratorTarget const* target,
                               std::string const& qtMajorVersion,
                               std::string const& config,
                               std::vector<std::string> const& configs,
                               AutogenSetup& setup)
{
  cmLocalGenerator* localGen = target->GetLocalGenerator();
  cmMakefile* makefile = target->Target->GetMakefile();

  AddDefinitionEscaped(makefile, "_moc_skip", setup.mocSkip);
  AddDefinitionEscaped(makefile, "_moc_options",
                       GetSafeProperty(target, "AUTOMOC_MOC_OPTIONS"));
  AddDefinitionEscaped(makefile, "_moc_relaxed_mode",
                       makefile->IsOn("CMAKE_AUTOMOC_RELAXED_MODE") ? "TRUE"
                                                                    : "FALSE");
  AddDefinitionEscaped(makefile, "_moc_macro_names",
                       GetSafeProperty(target, "AUTOMOC_MACRO_NAMES"));
  AddDefinitionEscaped(makefile, "_moc_depend_filters",
                       GetSafeProperty(target, "AUTOMOC_DEPEND_FILTERS"));

  if (QtVersionGreaterOrEqual(
        qtMajorVersion, GetQtMinorVersion(target, qtMajorVersion), 5, 8)) {
    AddDefinitionEscaped(
      makefile, "_moc_predefs_cmd",
      makefile->GetSafeDefinition("CMAKE_CXX_COMPILER_PREDEFINES_COMMAND"));
  }
  // Moc includes and compile definitions
  {
    // Default settings
    std::string incs;
    std::string compileDefs;
    GetCompileDefinitionsAndDirectories(target, config, incs, compileDefs);
    AddDefinitionEscaped(makefile, "_moc_incs", incs);
    AddDefinitionEscaped(makefile, "_moc_compile_defs", compileDefs);

    // Configuration specific settings
    for (std::vector<std::string>::const_iterator li = configs.begin();
         li != configs.end(); ++li) {
      std::string configIncs;
      std::string configCompileDefs;
      GetCompileDefinitionsAndDirectories(target, *li, configIncs,
                                          configCompileDefs);
      if (configIncs != incs) {
        setup.configMocIncludes[*li] = configIncs;
      }
      if (configCompileDefs != compileDefs) {
        setup.configMocDefines[*li] = configCompileDefs;
      }
    }
  }

  // Moc executable
  {
    std::string mocExec;
    std::string err;

    if (qtMajorVersion == "5") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt5::moc");
      if (tgt != CM_NULLPTR) {
        mocExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        err = "AUTOMOC: Qt5::moc target not found";
      }
    } else if (qtMajorVersion == "4") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt4::moc");
      if (tgt != CM_NULLPTR) {
        mocExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        err = "AUTOMOC: Qt4::moc target not found";
      }
    } else {
      err = "The AUTOMOC feature supports only Qt 4 and Qt 5";
    }

    if (err.empty()) {
      AddDefinitionEscaped(makefile, "_qt_moc_executable", mocExec);
    } else {
      err += " (" + target->GetName() + ")";
      cmSystemTools::Error(err.c_str());
    }
  }
}

static void UicGetOpts(cmGeneratorTarget const* target,
                       const std::string& config, std::string& optString)
{
  std::vector<std::string> opts;
  target->GetAutoUicOptions(opts, config);
  optString = cmJoin(opts, ";");
}

static void SetupAutoTargetUic(cmGeneratorTarget const* target,
                               std::string const& qtMajorVersion,
                               std::string const& config,
                               std::vector<std::string> const& configs,
                               AutogenSetup& setup)
{
  cmLocalGenerator* localGen = target->GetLocalGenerator();
  cmMakefile* makefile = target->Target->GetMakefile();

  AddDefinitionEscaped(makefile, "_uic_skip", setup.uicSkip);

  // Uic search paths
  {
    std::vector<std::string> uicSearchPaths;
    {
      const std::string usp = GetSafeProperty(target, "AUTOUIC_SEARCH_PATHS");
      if (!usp.empty()) {
        cmSystemTools::ExpandListArgument(usp, uicSearchPaths);
        const std::string srcDir = makefile->GetCurrentSourceDirectory();
        for (std::vector<std::string>::iterator it = uicSearchPaths.begin();
             it != uicSearchPaths.end(); ++it) {
          *it = cmSystemTools::CollapseFullPath(*it, srcDir);
        }
      }
    }
    AddDefinitionEscaped(makefile, "_uic_search_paths", uicSearchPaths);
  }
  // Uic target options
  {
    // Default settings
    std::string uicOpts;
    UicGetOpts(target, config, uicOpts);
    AddDefinitionEscaped(makefile, "_uic_target_options", uicOpts);

    // Configuration specific settings
    for (std::vector<std::string>::const_iterator li = configs.begin();
         li != configs.end(); ++li) {
      std::string configUicOpts;
      UicGetOpts(target, *li, configUicOpts);
      if (configUicOpts != uicOpts) {
        setup.configUicOptions[*li] = configUicOpts;
      }
    }
  }
  // Uic files options
  {
    std::vector<std::string> uiFileFiles;
    std::vector<std::string> uiFileOptions;
    {
      const std::set<std::string> skipped(setup.uicSkip.begin(),
                                          setup.uicSkip.end());

      const std::vector<cmSourceFile*> uiFilesWithOptions =
        makefile->GetQtUiFilesWithOptions();
      for (std::vector<cmSourceFile*>::const_iterator fileIt =
             uiFilesWithOptions.begin();
           fileIt != uiFilesWithOptions.end(); ++fileIt) {
        cmSourceFile* sf = *fileIt;
        const std::string absFile =
          cmsys::SystemTools::GetRealPath(sf->GetFullPath());
        if (skipped.find(absFile) == skipped.end()) {
          // The file wasn't skipped
          uiFileFiles.push_back(absFile);
          {
            std::string opts = sf->GetProperty("AUTOUIC_OPTIONS");
            cmSystemTools::ReplaceString(opts, ";",
                                         cmQtAutoGeneratorCommon::listSep);
            uiFileOptions.push_back(opts);
          }
        }
      }
    }
    AddDefinitionEscaped(makefile, "_qt_uic_options_files", uiFileFiles);
    AddDefinitionEscaped(makefile, "_qt_uic_options_options", uiFileOptions);
  }

  // Uic executable
  {
    std::string err;
    std::string uicExec;

    if (qtMajorVersion == "5") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt5::uic");
      if (tgt != CM_NULLPTR) {
        uicExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        // Project does not use Qt5Widgets, but has AUTOUIC ON anyway
      }
    } else if (qtMajorVersion == "4") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt4::uic");
      if (tgt != CM_NULLPTR) {
        uicExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        err = "AUTOUIC: Qt4::uic target not found";
      }
    } else {
      err = "The AUTOUIC feature supports only Qt 4 and Qt 5";
    }

    if (err.empty()) {
      AddDefinitionEscaped(makefile, "_qt_uic_executable", uicExec);
    } else {
      err += " (" + target->GetName() + ")";
      cmSystemTools::Error(err.c_str());
    }
  }
}

static std::string RccGetExecutable(cmGeneratorTarget const* target,
                                    const std::string& qtMajorVersion)
{
  std::string rccExec;
  std::string err;

  cmLocalGenerator* localGen = target->GetLocalGenerator();
  if (qtMajorVersion == "5") {
    cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt5::rcc");
    if (tgt != CM_NULLPTR) {
      rccExec = SafeString(tgt->ImportedGetLocation(""));
    } else {
      err = "AUTORCC: Qt5::rcc target not found";
    }
  } else if (qtMajorVersion == "4") {
    cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt4::rcc");
    if (tgt != CM_NULLPTR) {
      rccExec = SafeString(tgt->ImportedGetLocation(""));
    } else {
      err = "AUTORCC: Qt4::rcc target not found";
    }
  } else {
    err = "The AUTORCC feature supports only Qt 4 and Qt 5";
  }

  if (!err.empty()) {
    err += " (" + target->GetName() + ")";
    cmSystemTools::Error(err.c_str());
  }
  return rccExec;
}

static void RccMergeOptions(std::vector<std::string>& opts,
                            const std::vector<std::string>& fileOpts,
                            bool isQt5)
{
  static const char* valueOptions[] = { "name", "root", "compress",
                                        "threshold" };
  std::vector<std::string> extraOpts;
  for (std::vector<std::string>::const_iterator fit = fileOpts.begin();
       fit != fileOpts.end(); ++fit) {
    std::vector<std::string>::iterator existingIt =
      std::find(opts.begin(), opts.end(), *fit);
    if (existingIt != opts.end()) {
      const char* optName = fit->c_str();
      if (*optName == '-') {
        ++optName;
        if (isQt5 && *optName == '-') {
          ++optName;
        }
      }
      // Test if this is a value option and change the existing value
      if ((optName != fit->c_str()) &&
          std::find_if(cmArrayBegin(valueOptions), cmArrayEnd(valueOptions),
                       cmStrCmp(optName)) != cmArrayEnd(valueOptions)) {
        const std::vector<std::string>::iterator existValueIt(existingIt + 1);
        const std::vector<std::string>::const_iterator fileValueIt(fit + 1);
        if ((existValueIt != opts.end()) && (fileValueIt != fileOpts.end())) {
          *existValueIt = *fileValueIt;
          ++fit;
        }
      }
    } else {
      extraOpts.push_back(*fit);
    }
  }
  opts.insert(opts.end(), extraOpts.begin(), extraOpts.end());
}

static void SetupAutoTargetRcc(cmGeneratorTarget const* target,
                               const std::string& qtMajorVersion,
                               const std::vector<cmSourceFile*>& srcFiles)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  const bool qtMajorVersion5 = (qtMajorVersion == "5");
  const std::string rccCommand = RccGetExecutable(target, qtMajorVersion);
  std::vector<std::string> rccFiles;
  std::vector<std::string> rccInputs;
  std::vector<std::string> rccFileFiles;
  std::vector<std::string> rccFileOptions;
  std::vector<std::string> rccOptionsTarget;

  cmSystemTools::ExpandListArgument(GetSafeProperty(target, "AUTORCC_OPTIONS"),
                                    rccOptionsTarget);

  for (std::vector<cmSourceFile*>::const_iterator fileIt = srcFiles.begin();
       fileIt != srcFiles.end(); ++fileIt) {
    cmSourceFile* sf = *fileIt;
    // sf->GetExtension() is only valid after sf->GetFullPath() ...
    const std::string& fPath = sf->GetFullPath();
    if ((sf->GetExtension() == "qrc") &&
        !sf->GetPropertyAsBool("SKIP_AUTOGEN") &&
        !sf->GetPropertyAsBool("SKIP_AUTORCC")) {
      const std::string absFile = cmsys::SystemTools::GetRealPath(fPath);
      // qrc file
      rccFiles.push_back(absFile);
      // qrc file entries
      {
        std::string entriesList = "{";
        // Read input file list only for non generated .qrc files.
        if (!sf->GetPropertyAsBool("GENERATED")) {
          std::string error;
          std::vector<std::string> files;
          if (cmQtAutoGeneratorCommon::RccListInputs(
                qtMajorVersion, rccCommand, absFile, files, &error)) {
            entriesList += cmJoin(files, cmQtAutoGeneratorCommon::listSep);
          } else {
            cmSystemTools::Error(error.c_str());
          }
        }
        entriesList += "}";
        rccInputs.push_back(entriesList);
      }
      // rcc options for this qrc file
      {
        // Merged target and file options
        std::vector<std::string> rccOptions(rccOptionsTarget);
        if (const char* prop = sf->GetProperty("AUTORCC_OPTIONS")) {
          std::vector<std::string> optsVec;
          cmSystemTools::ExpandListArgument(prop, optsVec);
          RccMergeOptions(rccOptions, optsVec, qtMajorVersion5);
        }
        // Only store non empty options lists
        if (!rccOptions.empty()) {
          rccFileFiles.push_back(absFile);
          rccFileOptions.push_back(
            cmJoin(rccOptions, cmQtAutoGeneratorCommon::listSep));
        }
      }
    }
  }

  AddDefinitionEscaped(makefile, "_qt_rcc_executable", rccCommand);
  AddDefinitionEscaped(makefile, "_rcc_files", rccFiles);
  AddDefinitionEscaped(makefile, "_rcc_inputs", rccInputs);
  AddDefinitionEscaped(makefile, "_rcc_options_files", rccFileFiles);
  AddDefinitionEscaped(makefile, "_rcc_options_options", rccFileOptions);
}

void cmQtAutoGeneratorInitializer::InitializeAutogenTarget(
  cmLocalGenerator* localGen, cmGeneratorTarget* target)
{
  cmMakefile* makefile = target->Target->GetMakefile();

  // Create a custom target for running generators at buildtime
  const bool mocEnabled = target->GetPropertyAsBool("AUTOMOC");
  const bool uicEnabled = target->GetPropertyAsBool("AUTOUIC");
  const bool rccEnabled = target->GetPropertyAsBool("AUTORCC");
  const bool multiConfig = AutogenMultiConfig(target->GetGlobalGenerator());
  const std::string autogenTargetName = GetAutogenTargetName(target);
  const std::string autogenBuildDir = GetAutogenTargetBuildDir(target);
  const std::string workingDirectory =
    cmSystemTools::CollapseFullPath("", makefile->GetCurrentBinaryDirectory());
  const std::vector<std::string> suffixes = GetConfigurationSuffixes(makefile);
  std::set<std::string> autogenDependsSet;
  std::vector<std::string> autogenProvides;

  // Remove build directories on cleanup
  AddCleanFile(makefile, autogenBuildDir);

  // Remove old settings on cleanup
  {
    std::string base = GetAutogenTargetFilesDir(target);
    base += "/AutogenOldSettings";
    for (std::vector<std::string>::const_iterator it = suffixes.begin();
         it != suffixes.end(); ++it) {
      AddCleanFile(makefile, base + *it + ".cmake");
    }
  }

  // Compose command lines
  cmCustomCommandLines commandLines;
  {
    cmCustomCommandLine currentLine;
    currentLine.push_back(cmSystemTools::GetCMakeCommand());
    currentLine.push_back("-E");
    currentLine.push_back("cmake_autogen");
    currentLine.push_back(GetAutogenTargetFilesDir(target));
    currentLine.push_back("$<CONFIGURATION>");
    commandLines.push_back(currentLine);
  }

  // Compose target comment
  std::string autogenComment;
  {
    std::vector<std::string> toolNames;
    if (mocEnabled) {
      toolNames.push_back("MOC");
    }
    if (uicEnabled) {
      toolNames.push_back("UIC");
    }
    if (rccEnabled) {
      toolNames.push_back("RCC");
    }

    std::string tools = toolNames[0];
    toolNames.erase(toolNames.begin());
    while (toolNames.size() > 1) {
      tools += ", " + toolNames[0];
      toolNames.erase(toolNames.begin());
    }
    if (toolNames.size() == 1) {
      tools += " and " + toolNames[0];
    }
    autogenComment = "Automatic " + tools + " for target " + target->GetName();
  }

  // Add moc compilation to generated files list
  if (mocEnabled) {
    const std::string mocsComp = autogenBuildDir + "/mocs_compilation.cpp";
    AddGeneratedSource(target, mocsComp, cmQtAutoGeneratorCommon::MOC);
    autogenProvides.push_back(mocsComp);
  }

  // Add autogen includes directory to the origin target INCLUDE_DIRECTORIES
  if (mocEnabled || uicEnabled) {
    std::string includeDir = autogenBuildDir + "/include";
    if (multiConfig) {
      includeDir += "_$<CONFIG>";
    }
    target->AddIncludeDirectory(includeDir, true);
  }

#if defined(_WIN32) && !defined(__CYGWIN__)
  bool usePRE_BUILD = false;
  cmGlobalGenerator* gg = localGen->GetGlobalGenerator();
  if (gg->GetName().find("Visual Studio") != std::string::npos) {
    // Under VS use a PRE_BUILD event instead of a separate target to
    // reduce the number of targets loaded into the IDE.
    // This also works around a VS 11 bug that may skip updating the target:
    //  https://connect.microsoft.com/VisualStudio/feedback/details/769495
    usePRE_BUILD = true;
  }
#endif

  // Add user defined autogen target dependencies
  {
    const std::string deps = GetSafeProperty(target, "AUTOGEN_TARGET_DEPENDS");
    if (!deps.empty()) {
      std::vector<std::string> extraDepends;
      cmSystemTools::ExpandListArgument(deps, extraDepends);
      autogenDependsSet.insert(extraDepends.begin(), extraDepends.end());
    }
  }
  // Add utility target dependencies to the autogen dependencies
  {
    const std::set<std::string>& utils = target->Target->GetUtilities();
    for (std::set<std::string>::const_iterator it = utils.begin();
         it != utils.end(); ++it) {
      const std::string& targetName = *it;
      if (makefile->FindTargetToUse(targetName) != CM_NULLPTR) {
        autogenDependsSet.insert(targetName);
      }
    }
  }
  // Add link library target dependencies to the autogen dependencies
  {
    const cmTarget::LinkLibraryVectorType& libVec =
      target->Target->GetOriginalLinkLibraries();
    for (cmTarget::LinkLibraryVectorType::const_iterator it = libVec.begin();
         it != libVec.end(); ++it) {
      const std::string& libName = it->first;
      if (makefile->FindTargetToUse(libName) != CM_NULLPTR) {
        autogenDependsSet.insert(libName);
      }
    }
  }

  // Extract relevant source files
  std::vector<std::string> generatedSources;
  std::vector<std::pair<std::string, bool> > qrcSources;
  {
    const std::string qrcExt = "qrc";
    std::vector<cmSourceFile*> srcFiles;
    target->GetConfigCommonSourceFiles(srcFiles);
    for (std::vector<cmSourceFile*>::const_iterator fileIt = srcFiles.begin();
         fileIt != srcFiles.end(); ++fileIt) {
      cmSourceFile* sf = *fileIt;
      if (sf->GetPropertyAsBool("SKIP_AUTOGEN")) {
        continue;
      }
      // sf->GetExtension() is only valid after sf->GetFullPath() ...
      const std::string& fPath = sf->GetFullPath();
      const std::string& ext = sf->GetExtension();
      // Register generated files that will be scanned by moc or uic
      if (mocEnabled || uicEnabled) {
        const cmSystemTools::FileFormat fileType =
          cmSystemTools::GetFileFormat(ext.c_str());
        if ((fileType == cmSystemTools::CXX_FILE_FORMAT) ||
            (fileType == cmSystemTools::HEADER_FILE_FORMAT)) {
          if (sf->GetPropertyAsBool("GENERATED")) {
            if ((mocEnabled && !sf->GetPropertyAsBool("SKIP_AUTOMOC")) ||
                (uicEnabled && !sf->GetPropertyAsBool("SKIP_AUTOUIC"))) {
              generatedSources.push_back(
                cmsys::SystemTools::GetRealPath(fPath));
            }
          }
        }
      }
      // Register rcc enabled files
      if (rccEnabled && (ext == qrcExt) &&
          !sf->GetPropertyAsBool("SKIP_AUTORCC")) {
        qrcSources.push_back(
          std::pair<std::string, bool>(cmsys::SystemTools::GetRealPath(fPath),
                                       sf->GetPropertyAsBool("GENERATED")));
      }
    }
    // cmGeneratorTarget::GetConfigCommonSourceFiles computes the target's
    // sources meta data cache. Clear it so that OBJECT library targets that
    // are AUTOGEN initialized after this target get their added
    // mocs_compilation.cpp source acknowledged by this target.
    target->ClearSourcesCache();
  }

  if (!generatedSources.empty()) {
    for (std::vector<std::string>::const_iterator it =
           generatedSources.begin();
         it != generatedSources.end(); ++it) {
      autogenDependsSet.insert(*it);
    }
  }

  if (!qrcSources.empty()) {
    const std::string qtMajorVersion = GetQtMajorVersion(target);
    const std::string rccCommand = RccGetExecutable(target, qtMajorVersion);
    const cmFilePathChecksum fpathCheckSum(makefile);
    for (std::vector<std::pair<std::string, bool> >::const_iterator it =
           qrcSources.begin();
         it != qrcSources.end(); ++it) {
      const std::string& absFile = it->first;

      // Compose rcc output file name
      {
        std::string rccBuildFile = autogenBuildDir + "/";
        rccBuildFile += fpathCheckSum.getPart(absFile);
        rccBuildFile += "/qrc_";
        rccBuildFile +=
          cmsys::SystemTools::GetFilenameWithoutLastExtension(absFile);
        rccBuildFile += ".cpp";

        // Register rcc ouput file as generated
        AddGeneratedSource(target, rccBuildFile, cmQtAutoGeneratorCommon::RCC);
        // Register rcc ouput file as generated by the _autogen target
        autogenProvides.push_back(rccBuildFile);
      }

      if (it->second) {
        // Add generated qrc file to the dependencies
        autogenDependsSet.insert(absFile);
      } else {
        // Run cmake again when .qrc file changes
        makefile->AddCMakeDependFile(absFile);
        // Add the qrc input files to the dependencies
        {
          std::string error;
          std::vector<std::string> extraDepends;
          if (cmQtAutoGeneratorCommon::RccListInputs(
                qtMajorVersion, rccCommand, absFile, extraDepends, &error)) {
            autogenDependsSet.insert(extraDepends.begin(), extraDepends.end());
          } else {
            cmSystemTools::Error(error.c_str());
          }
        }
      }
    }
  }

  // Convert std::set to std::vector
  const std::vector<std::string> autogenDepends(autogenDependsSet.begin(),
                                                autogenDependsSet.end());
#if defined(_WIN32) && !defined(__CYGWIN__)
  if (usePRE_BUILD) {
    if (!generatedSources.empty() || !qrcSources.empty()) {
      // - Cannot use PRE_BUILD with generated files
      // - Cannot use PRE_BUILD because the resource files themselves
      // may not be sources within the target so VS may not know the
      // target needs to re-build at all.
      usePRE_BUILD = false;
    }
  }
  if (usePRE_BUILD) {
    // If the autogen target depends on an other target don't use PRE_BUILD
    for (std::vector<std::string>::const_iterator it = autogenDepends.begin();
         it != autogenDepends.end(); ++it) {
      if (makefile->FindTargetToUse(*it) != CM_NULLPTR) {
        usePRE_BUILD = false;
        break;
      }
    }
  }
  if (usePRE_BUILD) {
    // Add the pre-build command directly to bypass the OBJECT_LIBRARY
    // rejection in cmMakefile::AddCustomCommandToTarget because we know
    // PRE_BUILD will work for an OBJECT_LIBRARY in this specific case.
    std::vector<std::string> no_output;
    cmCustomCommand cc(makefile, no_output, autogenProvides, autogenDepends,
                       commandLines, autogenComment.c_str(),
                       workingDirectory.c_str());
    cc.SetEscapeOldStyle(false);
    cc.SetEscapeAllowMakeVars(true);
    target->Target->AddPreBuildCommand(cc);
  } else
#endif
  {
    cmTarget* autogenTarget = makefile->AddUtilityCommand(
      autogenTargetName, true, workingDirectory.c_str(),
      /*byproducts=*/autogenProvides, autogenDepends, commandLines, false,
      autogenComment.c_str());

    localGen->AddGeneratorTarget(
      new cmGeneratorTarget(autogenTarget, localGen));

    // Set autogen target FOLDER
    {
      const char* autogenFolder =
        makefile->GetState()->GetGlobalProperty("AUTOMOC_TARGETS_FOLDER");
      if (autogenFolder == CM_NULLPTR) {
        autogenFolder =
          makefile->GetState()->GetGlobalProperty("AUTOGEN_TARGETS_FOLDER");
      }
      // Inherit FOLDER property from target (#13688)
      if (autogenFolder == CM_NULLPTR) {
        autogenFolder = target->Target->GetProperty("FOLDER");
      }
      if ((autogenFolder != CM_NULLPTR) && (*autogenFolder != '\0')) {
        autogenTarget->SetProperty("FOLDER", autogenFolder);
      }
    }

    // Add autogen target to the origin target dependencies
    target->Target->AddUtility(autogenTargetName);
  }
}

void cmQtAutoGeneratorInitializer::SetupAutoGenerateTarget(
  cmGeneratorTarget const* target)
{
  cmMakefile* makefile = target->Target->GetMakefile();

  // forget the variables added here afterwards again:
  cmMakefile::ScopePushPop varScope(makefile);
  static_cast<void>(varScope);

  // Get configurations
  std::string config;
  const std::vector<std::string> configs(GetConfigurations(makefile, &config));

  // Configuration suffixes
  std::map<std::string, std::string> configSuffix;
  if (AutogenMultiConfig(target->GetGlobalGenerator())) {
    for (std::vector<std::string>::const_iterator it = configs.begin();
         it != configs.end(); ++it) {
      configSuffix[*it] = "_" + *it;
    }
  }

  // Configurations settings buffers
  AutogenSetup setup;

  // Basic setup
  {
    const bool mocEnabled = target->GetPropertyAsBool("AUTOMOC");
    const bool uicEnabled = target->GetPropertyAsBool("AUTOUIC");
    const bool rccEnabled = target->GetPropertyAsBool("AUTORCC");
    const std::string qtMajorVersion = GetQtMajorVersion(target);
    {
      std::vector<cmSourceFile*> srcFiles;
      target->GetConfigCommonSourceFiles(srcFiles);
      if (mocEnabled || uicEnabled) {
        SetupAcquireScanFiles(target, mocEnabled, uicEnabled, srcFiles, setup);
        if (mocEnabled) {
          SetupAutoTargetMoc(target, qtMajorVersion, config, configs, setup);
        }
        if (uicEnabled) {
          SetupAutoTargetUic(target, qtMajorVersion, config, configs, setup);
        }
      }
      if (rccEnabled) {
        SetupAutoTargetRcc(target, qtMajorVersion, srcFiles);
      }
    }

    AddDefinitionEscaped(makefile, "_build_dir",
                         GetAutogenTargetBuildDir(target));
    AddDefinitionEscaped(makefile, "_qt_version_major", qtMajorVersion);
    AddDefinitionEscaped(makefile, "_sources", setup.sources);
    AddDefinitionEscaped(makefile, "_headers", setup.headers);
  }

  // Generate info file
  std::string infoFile = GetAutogenTargetFilesDir(target);
  infoFile += "/AutogenInfo.cmake";
  {
    std::string inf = cmSystemTools::GetCMakeRoot();
    inf += "/Modules/AutogenInfo.cmake.in";
    makefile->ConfigureFile(inf.c_str(), infoFile.c_str(), false, true, false);
  }

  // Append custom definitions to info file on demand
  if (!configSuffix.empty() || !setup.configMocDefines.empty() ||
      !setup.configMocIncludes.empty() || !setup.configUicOptions.empty()) {

    // Ensure we have write permission in case .in was read-only.
    mode_t perm = 0;
#if defined(_WIN32) && !defined(__CYGWIN__)
    mode_t mode_write = S_IWRITE;
#else
    mode_t mode_write = S_IWUSR;
#endif
    cmSystemTools::GetPermissions(infoFile, perm);
    if (!(perm & mode_write)) {
      cmSystemTools::SetPermissions(infoFile, perm | mode_write);
    }

    // Open and write file
    cmsys::ofstream ofs(infoFile.c_str(), std::ios::app);
    if (ofs) {
      ofs << "# Configuration specific options\n";
      for (std::map<std::string, std::string>::iterator
             it = configSuffix.begin(),
             end = configSuffix.end();
           it != end; ++it) {
        ofs << "set(AM_CONFIG_SUFFIX_" << it->first << " "
            << cmOutputConverter::EscapeForCMake(it->second) << ")\n";
      }
      for (std::map<std::string, std::string>::iterator
             it = setup.configMocDefines.begin(),
             end = setup.configMocDefines.end();
           it != end; ++it) {
        ofs << "set(AM_MOC_DEFINITIONS_" << it->first << " "
            << cmOutputConverter::EscapeForCMake(it->second) << ")\n";
      }
      for (std::map<std::string, std::string>::iterator
             it = setup.configMocIncludes.begin(),
             end = setup.configMocIncludes.end();
           it != end; ++it) {
        ofs << "set(AM_MOC_INCLUDES_" << it->first << " "
            << cmOutputConverter::EscapeForCMake(it->second) << ")\n";
      }
      for (std::map<std::string, std::string>::iterator
             it = setup.configUicOptions.begin(),
             end = setup.configUicOptions.end();
           it != end; ++it) {
        ofs << "set(AM_UIC_TARGET_OPTIONS_" << it->first << " "
            << cmOutputConverter::EscapeForCMake(it->second) << ")\n";
      }
    } else {
      // File open error
      std::string error = "Internal CMake error when trying to open file: ";
      error += cmQtAutoGeneratorCommon::Quoted(infoFile);
      error += " for writing.";
      cmSystemTools::Error(error.c_str());
    }
  }
}

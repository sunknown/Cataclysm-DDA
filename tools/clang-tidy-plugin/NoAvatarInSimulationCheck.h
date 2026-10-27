#ifndef CATA_TOOLS_CLANG_TIDY_PLUGIN_NOAVATARINSIMULATIONCHECK_H
#define CATA_TOOLS_CLANG_TIDY_PLUGIN_NOAVATARINSIMULATIONCHECK_H

#include <string>

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace clang
{

namespace tidy
{
class ClangTidyContext;

namespace cata
{

// Forbids get_avatar() and get_player_character() in the simulation files
// that have already been migrated to the multi-player accessors. Calls that
// keep the single-player meaning on purpose are spelled get_avatar_legacy().
class NoAvatarInSimulationCheck : public ClangTidyCheck
{
    public:
        NoAvatarInSimulationCheck( StringRef Name, ClangTidyContext *Context );
        void storeOptions( ClangTidyOptions::OptionMap &Opts ) override;
        void registerMatchers( ast_matchers::MatchFinder *Finder ) override;
        void check( const ast_matchers::MatchFinder::MatchResult &Result ) override;
    private:
        // Semicolon-separated file names, matched against the basename.
        const std::string SimulationFilesOption;
        SmallVector<StringRef> SimulationFiles;
};

} // namespace cata
} // namespace tidy
} // namespace clang

#endif // CATA_TOOLS_CLANG_TIDY_PLUGIN_NOAVATARINSIMULATIONCHECK_H

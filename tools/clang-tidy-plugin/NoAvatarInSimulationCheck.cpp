#include "NoAvatarInSimulationCheck.h"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Path.h>

using namespace clang::ast_matchers;

namespace clang::tidy::cata
{

// The simulation files whose player access has been migrated: every remaining
// local-player use there is already spelled get_avatar_legacy(). A file joins
// this list once its last direct call is gone.
static constexpr const char *DefaultSimulationFiles =
    "creature.cpp;map.cpp;mapgen.cpp;monattack.cpp;monmove.cpp;monster.cpp;npc.cpp;"
    "npcmove.cpp;overmapbuffer.cpp;weather.cpp";

NoAvatarInSimulationCheck::NoAvatarInSimulationCheck( StringRef Name,
        ClangTidyContext *Context )
    : ClangTidyCheck( Name, Context )
    , SimulationFilesOption( Options.get( "SimulationFiles", DefaultSimulationFiles ) )
{
    StringRef( SimulationFilesOption ).split( SimulationFiles, ';', -1, false );
}

void NoAvatarInSimulationCheck::storeOptions( ClangTidyOptions::OptionMap &Opts )
{
    Options.store( Opts, "SimulationFiles", SimulationFilesOption );
}

void NoAvatarInSimulationCheck::registerMatchers( MatchFinder *Finder )
{
    // A reference rather than a call, so that taking the function's address
    // is caught as well.
    Finder->addMatcher(
        declRefExpr(
            to( functionDecl( hasAnyName( "::get_avatar", "::get_player_character" ) ) )
        ).bind( "ref" ), this );
}

void NoAvatarInSimulationCheck::check( const MatchFinder::MatchResult &Result )
{
    const DeclRefExpr *Ref = Result.Nodes.getNodeAs<DeclRefExpr>( "ref" );
    if( !Ref ) {
        return;
    }
    const SourceManager &SM = *Result.SourceManager;
    const StringRef Filename = SM.getFilename( SM.getExpansionLoc( Ref->getBeginLoc() ) );
    if( !llvm::is_contained( SimulationFiles, llvm::sys::path::filename( Filename ) ) ) {
        return;
    }
    diag( Ref->getBeginLoc(),
          "%0 is whichever player is active, and simulation code also runs in the world "
          "phase, where that is always slot 0.  Use a multi-player accessor such as "
          "game::nearest_player, or get_avatar_legacy() to keep the single-player meaning "
          "on purpose." ) << Ref->getDecl();
}

} // namespace clang::tidy::cata

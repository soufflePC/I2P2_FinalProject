#include <algorithm>
#include <utility>

#include "state.hpp"
#include "random.hpp"
#include "registry.hpp"

constexpr int QUIESCENCE_MAX_PLY = 6;
constexpr int MVV_LVA_VALUES[7] = {0, 10, 50, 30, 30, 90, 900};

bool is_capture(State *state, const Move& move){
    int to_row = move.second.first;
    int to_col = move.second.second;
    return state->piece_at(1 - state->player, to_row, to_col) > 0;
}

int move_score(State *state, const Move& move){
    int from_row = move.first.first;
    int from_col = move.first.second;
    int to_row = move.second.first;
    int to_col = move.second.second;

    int attacker = state->piece_at(state->player, from_row, from_col);
    int victim = state->piece_at(1 - state->player, to_row, to_col);

    int score = 0;
    if(victim > 0){
        score += (
            10000
            + MVV_LVA_VALUES[victim] * 16
            - MVV_LVA_VALUES[attacker]
        );
    }
    if(attacker == 1 && (to_row == 0 || to_row == state->board_h() - 1)){
        score += 800;
    }
    return score;
}

std::vector<Move> ordered_moves(State *state, bool captures_only = false){
    std::vector<Move> moves;
    moves.reserve(state->legal_actions.size());

    for(const auto& move : state->legal_actions){
        if(!captures_only || is_capture(state, move)){
            moves.push_back(move);
        }
    }

    std::stable_sort(moves.begin(), moves.end(), [&](const Move& a, const Move& b){
        return move_score(state, a) > move_score(state, b);
    });

    return moves;
}

int quiescence_after_push(
    State *state,
    GameHistory& history,
    int ply,
    int qply,
    SearchContext& ctx,
    const RParams& p,
    int alpha,
    int beta
);

int eval_impl(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const RParams& p,
    int alpha,
    int beta
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    history.push(state->hash());

    if(depth <= 0){
        int score = quiescence_after_push(
            state, history, ply, 0, ctx, p, alpha, beta
        );
        history.pop(state->hash());
        return score;
    }

    int best_score = M_MAX;
    auto moves = ordered_moves(state);

    for(const auto& action : moves){
        State *next = state->next_state(action);
        bool same_player = next->same_player_as_parent();
        int score = same_player
            ? eval_impl(next, depth - 1, history, ply + 1, ctx, p, alpha, beta)
            : -eval_impl(next, depth - 1, history, ply + 1, ctx, p, -beta, -alpha);
        delete next;

        if(score > best_score){
            best_score = score;
        }
        if(score > alpha){
            alpha = score;
        }
        if(alpha >= beta || ctx.stop){
            break;
        }
    }

    history.pop(state->hash());
    return best_score;
}

int quiescence_after_push(
    State *state,
    GameHistory& history,
    int ply,
    int qply,
    SearchContext& ctx,
    const RParams& p,
    int alpha,
    int beta
){
    if(ctx.stop){
        return 0;
    }
    if(ply + qply > ctx.seldepth){
        ctx.seldepth = ply + qply;
    }

    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta || qply >= QUIESCENCE_MAX_PLY){
        return stand_pat;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    auto captures = ordered_moves(state, true);
    for(const auto& action : captures){
        State *next = state->next_state(action);
        bool same_player = next->same_player_as_parent();
        int score = 0;
        ctx.nodes++;

        if(next->legal_actions.empty() && next->game_state == UNKNOWN){
            next->get_legal_actions();
        }

        if(next->game_state == WIN){
            score = same_player ? P_MAX - (ply + qply + 1) : M_MAX + (ply + qply + 1);
        }else if(next->game_state == DRAW){
            score = 0;
        }else{
            int rep_score;
            if(next->check_repetition(history, rep_score)){
                score = rep_score;
            }else{
                history.push(next->hash());
                score = same_player
                    ? quiescence_after_push(
                        next, history, ply, qply + 1, ctx, p, alpha, beta
                    )
                    : -quiescence_after_push(
                        next, history, ply, qply + 1, ctx, p, -beta, -alpha
                    );
                history.pop(next->hash());
            }
        }

        delete next;

        if(score > alpha){
            alpha = score;
        }
        if(alpha >= beta || ctx.stop){
            break;
        }
    }

    return alpha;
}

int Random::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const RParams& p
){
    return eval_impl(state, depth, history, ply, ctx, p, M_MAX, P_MAX);
}


/*============================================================
 *  - search
 *
 * Iterate ordered legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult Random::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    RParams p = RParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = state->legal_actions.size();
    auto moves = ordered_moves(state);

    for(auto& action : moves){
        State *next = state->next_state(action);
        bool same_player = next->same_player_as_parent();
        int score = same_player
            ? eval_impl(next, depth - 1, history, 1, ctx, p, best_score, P_MAX)
            : -eval_impl(next, depth - 1, history, 1, ctx, p, M_MAX, -best_score);
        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;

            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({
                    result.best_move,
                    best_score,
                    depth,
                    move_index + 1,
                    total_moves
                });
            }
        }
        move_index++;
    }

    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    if(result.best_move == Move() && !moves.empty()){
        result.best_move = moves.front();
    }
    if(!moves.empty()){
        result.pv.push_back(result.best_move);
    }
    return result;
}


/*============================================================
 *  - default_params / param_defs
 *============================================================*/
ParamMap Random::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> Random::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}

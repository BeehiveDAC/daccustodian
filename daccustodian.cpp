#include <eosiolib/eosio.hpp>
#include <eosiolib/singleton.hpp>
#include <eosiolib/asset.hpp>
#include <string>

#include "daccustodian.hpp"

using namespace eosio;
using namespace std;
using eosio::print;

class daccustodian : public contract {

private:
    configscontainer config_singleton;
    candidates_table registered_candidates;
    votes_table votes_cast_by_members;
    pending_pay_table pending_pay;
    regmembers reg_members;
    memterms memberterms;

    symbol_type EOSDACSYMBOL = eosio::symbol_type(eosio::string_to_symbol(4, "EOSDAC"));
    symbol_type PAYMENT_TOKEN = eosio::symbol_type(eosio::string_to_symbol(4, "EOS"));


    contract_config configs() {
        contract_config conf = config_singleton.get_or_default(contract_config());
        config_singleton.set(conf, _self);
        return conf;
    }

    member get_valid_member(name member) {
        const auto &regmem = reg_members.get(member, "Account is not registered with members");
        eosio_assert((regmem.agreedterms != 0), "Account has not agreed to any terms");
        auto latest_member_terms = (--memberterms.end());
        eosio_assert( latest_member_terms->version == regmem.agreedterms, "Agreed terms isn't the latest." );
        return regmem;
    }

public:

    daccustodian(account_name self)
            : contract(self),
              registered_candidates(_self, _self),
              votes_cast_by_members(_self, _self),
              pending_pay(_self, _self),
              config_singleton(_self, _self),
              reg_members(N(eosdactoken), N(eosdactoken)),
              memberterms(N(eosdactoken), N(eosdactoken)) {}

    void updateconfig(asset lockupasset, uint8_t maxvotes, uint32_t numelected) {
        require_auth(_self);
        eosio_assert(lockupasset.symbol == configs().lockupasset.symbol,
                     "The provided asset does not match the current lockup asset symbol.");
        contract_config newconfig{lockupasset, maxvotes};
        config_singleton.set(newconfig, _self);
    }

    void regcandidate(name cand, string bio, asset requestedpay) {

        require_auth(cand);
        get_valid_member(cand);
        account_name tokencontract = N(eosdactoken);

        auto reg_candidate = registered_candidates.find(cand);
        eosio_assert(reg_candidate == registered_candidates.end(), "Candidate is already registered.");
        eosio_assert(requestedpay.symbol == PAYMENT_TOKEN, "Candidate is already registered.");

        action(permission_level{cand, N(active)},
               N(eosdactoken), N(transfer),
               std::make_tuple(cand, _self, configs().lockupasset, std::string("Candidate lockup amount"))
        ).send();

        registered_candidates.emplace(_self, [&](candidate &c) {
            c.candidate_name = cand;
            c.bio = bio;
            c.requestedpay = requestedpay;
            c.pendreqpay = asset(0, PAYMENT_TOKEN);
            c.is_custodian = false;
            c.locked_tokens = configs().lockupasset;
            c.total_votes = 0;
        });
    }

    void unregcand(name cand) {

        require_auth(cand);
        const auto &reg_candidate = registered_candidates.get(cand, "Candidate is not already registered.");

        registered_candidates.erase(reg_candidate);

        pending_pay.emplace(_self, [&](pay &p) {
            p.receiver = cand;
            p.quantity = reg_candidate.locked_tokens;
            p.memo = "Returning locked up stake. Thank you.";
        });
    }

    void updatebio(name cand, string bio) {

        require_auth(cand);
        get_valid_member(cand);

        const auto &reg_candidate = registered_candidates.get(cand, "Candidate is not already registered.");

        registered_candidates.modify(reg_candidate, 0, [&](candidate &c) {
            c.bio = bio;
        });
    }

    void updatereqpay(name cand, asset requestedpay) {

        require_auth(cand);
        get_valid_member(cand);
        const auto &reg_candidate = registered_candidates.get(cand, "Candidate is not already registered.");

        registered_candidates.modify(reg_candidate, 0, [&](candidate &c) {
            c.pendreqpay = requestedpay;
        });
    }

    void votecust(name voter, vector<name> newvotes) {

        require_auth(voter);
        get_valid_member(voter);

        eosio_assert(newvotes.size() <= configs().maxvotes, "Max number of allowed votes was exceeded.");

        // Find a vote that has been cast by this voter previously.
        auto existingVote = votes_cast_by_members.find(voter);
        if (existingVote != votes_cast_by_members.end()) {

            votes_cast_by_members.modify(existingVote, _self, [&](vote &v) {
                v.candidates = newvotes;
                v.proxy = name();
            });
        } else {
            votes_cast_by_members.emplace(_self, [&](vote &v) {
                v.voter = voter;
                v.candidates = newvotes;
            });
        }
    }

    void voteproxy(name voter, name proxy) {

        require_auth(voter);
        get_valid_member(voter);

        string error_msg = "Member cannot proxy vote for themselves: " + voter.to_string();
        eosio_assert(voter != proxy, error_msg.c_str());
        auto destproxy = votes_cast_by_members.find(proxy);
        if (destproxy != votes_cast_by_members.end()) {
            error_msg = "Proxy voters cannot vote for another proxy: " + voter.to_string();
            eosio_assert(destproxy->proxy == 0, error_msg.c_str());
        }

        // Find a vote that has been cast by this voter previously.
        auto existingVote = votes_cast_by_members.find(voter);
        if (existingVote != votes_cast_by_members.end()) {

            votes_cast_by_members.modify(existingVote, _self, [&](vote &v) {
                v.candidates.clear();
                v.proxy = proxy;
            });
        } else {
            votes_cast_by_members.emplace(_self, [&](vote &v) {
                v.voter = voter;
                v.proxy = proxy;
            });
        }
    }

    void newperiod(string message) {
        require_auth(_self);

        /* Copied from the Tech Doc vvvvv
         // 1. Distribute custodian pay based on the median of requested pay for all currently elected candidates

         // 2. Tally the current votes_cast_by_members and prepare a list of the winning custodians
         // 3. Assigns the custodians, this may include updating a multi-sig wallet which controls the funds in the DAC as well as updating DAC contract code
         * Copied from the Tech Doc ^^^^^
         */

        // These actions a separated out for clarity and incase we want to be able to call them individually the change would be minimal.
        distributepay();
        clearOldVotes();
        tallyNewVotes();
        configureForNextPeriod();
    }

    void paypending(string message) {
        require_auth(_self);
        auto payidx = pending_pay.begin();

        while (payidx != pending_pay.end()) {
            if (payidx->quantity.symbol == PAYMENT_TOKEN) {
                action(permission_level{_self, N(active)},
                       N(eosio.token), N(transfer),
                       std::make_tuple(_self, payidx->receiver, payidx->quantity, payidx->memo)
                ).send();
            } else {

                action(permission_level{_self, N(active)},
                       N(eosdactoken), N(transfer),
                       std::make_tuple(_self, payidx->receiver, payidx->quantity, payidx->memo)
                ).send();
            }

            payidx = pending_pay.erase(payidx);
        }
    }

private:
    void distributepay() {
        auto idx = registered_candidates.get_index<N(isvotedpay)>();
        auto it = idx.rbegin();

        //Find the median pay using a temporary vector to hold the requestedpay amounts.
        std::vector<int64_t> reqpays;
        while (it != idx.rend()) {
            if (it->is_custodian) {
                reqpays.push_back(it->requestedpay.amount);
            }
            it++;
        }

        // Using nth_element to just sort for the entry we need for the median value.
        size_t mid = reqpays.size() / 2;
        std::nth_element(reqpays.begin(), reqpays.begin() + mid, reqpays.end());

        asset median = asset(reqpays[mid], PAYMENT_TOKEN);

        it = idx.rbegin();
        while (it != idx.rend()) {
            if (it->is_custodian) {
                auto currentPay = pending_pay.find(it->candidate_name);
                if (currentPay != pending_pay.end()) {
                    pending_pay.modify(currentPay, _self, [&](pay &p) {
                        p.quantity += median;
                    });

                } else {
                    pending_pay.emplace(_self, [&](pay &p) {
                        p.receiver = it->candidate_name;
                        p.quantity = median;
                        p.memo = "EOSDAC Custodian pay. Thank you.";
                    });
                }
            }
            ++it;
        }
    }

    void clearOldVotes() {
        auto voteitr = votes_cast_by_members.begin();
        while (voteitr != votes_cast_by_members.end()) {
            votes_cast_by_members.modify(*voteitr, _self, [&](vote &v) {
                v.weight = 0;
            });
            voteitr++;
        }

        auto canditr = registered_candidates.begin();
        while (canditr != registered_candidates.end()) {
            registered_candidates.modify(*canditr, _self, [&](candidate &c) {
                c.total_votes = 0;
            });
            canditr++;
        }
    }

    void tallyNewVotes() {
        auto byProxyIdx = votes_cast_by_members.get_index<N(byproxy)>();
        uint64_t asset_name = configs().lockupasset.symbol.name();

        auto itr = byProxyIdx.rbegin();
        auto end = byProxyIdx.rend();

        // This should go iterate through proxy votes first to increase the proxy weight factor.
        // Therefore the sorting order is important here.
        while (itr != end) {
            accounts accountstable(N(eosdactoken), itr->voter);
            const auto ac = accountstable.find(asset_name);
            if (ac != accountstable.end()) {
                int64_t vote_weight = ac->balance.amount;

                votes_cast_by_members.modify(*itr, _self, [&](vote &v) {
                    v.weight += vote_weight;
                });

                if (itr->proxy != 0) {
                    auto proxied_to_voter = votes_cast_by_members.find(itr->proxy); // else "no active vote for proxy");
                    if (proxied_to_voter != votes_cast_by_members.end()) {
                        votes_cast_by_members.modify(proxied_to_voter, _self, [&](vote &p) {
                            p.weight += vote_weight;
                        });
                    }
                }
            } else {
                // Voter as no balance - Ignoring this error since the impact is small;
            }
            if (itr->proxy == 0) {
                for (const auto &newVote : itr->candidates) {
                    auto candidate = registered_candidates.find(newVote);
                    registered_candidates.modify(candidate, _self, [&](auto &c) {
                        c.total_votes += itr->weight;
                    });
                }
            }
            ++itr;
        }
    }

    void configureForNextPeriod() {
        auto isvotedpayidx = registered_candidates.get_index<N(byvotes)>();
        auto it = isvotedpayidx.rbegin();
        auto end = isvotedpayidx.rend();

        int i = 0;
        int32_t electcount = configs().numelected;
        while (it != end) {
            registered_candidates.modify(*it, _self, [&](candidate &cand) {
                cand.is_custodian = i < electcount ? 1 : 0; // Set elected to the highest number of votes.
                if (cand.pendreqpay.amount > 0) {
                    // Move the pending request pay to the request pay for the next period.
                    cand.requestedpay = cand.pendreqpay;
                    // zeros the pending request to prevent overwrite of requestedPay on the next cycle.
                    cand.pendreqpay = asset(0, PAYMENT_TOKEN);
                }
            });
            ++it;
            ++i;
        }
    }

};

EOSIO_ABI(daccustodian,
          (updateconfig)(regcandidate)(unregcand)(updatebio)(updatereqpay)(votecust)(voteproxy)(newperiod)(paypending))

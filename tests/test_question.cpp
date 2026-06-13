#include <string>
#include <thread>
#include <vector>

#include "agent/question_tool.hpp"
#include "test_harness.hpp"

using namespace flagent;

// --- QuestionGate synchronization -----------------------------------------

TEST("question_gate: request then answer") {
    QuestionGate gate;

    std::string answer;
    std::thread worker([&] {
        auto r = gate.request("pick one", {"A", "B", "C"});
        if (r) answer = *r;
        else answer = "<halted>";
    });

    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(gate.pending());
    auto q = gate.pending_question();
    CHECK(q.has_value());
    CHECK_EQ(q->question, std::string("pick one"));
    CHECK(q->options.size() == 3u);
    CHECK_EQ(q->options[0], std::string("A"));

    gate.answer("B");
    worker.join();
    CHECK_EQ(answer, std::string("B"));
    CHECK(!gate.pending());
}

TEST("question_gate: halt returns nullopt") {
    QuestionGate gate;

    std::optional<std::string> result;
    std::thread worker([&] { result = gate.request("q", {"x"}); });

    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(gate.pending());
    gate.halt();
    worker.join();
    CHECK(!result.has_value());
    CHECK(!gate.pending());
}

TEST("question_gate: released gate rejects immediately") {
    QuestionGate gate;
    gate.release();

    auto r = gate.request("q", {"a"});
    CHECK(!r.has_value());
    CHECK(!gate.pending());
}

TEST("question_gate: release unblocks waiting request") {
    QuestionGate gate;

    std::optional<std::string> result;
    std::thread worker([&] { result = gate.request("q", {"x"}); });

    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(gate.pending());
    gate.release();
    worker.join();
    CHECK(!result.has_value());
}

// --- ask_user tool arg parsing --------------------------------------------

TEST("ask_user: parses valid args") {
    auto tool = ask_user_tool(nullptr);  // nullptr → stdin path
    CHECK_EQ(tool.spec.name, std::string("ask_user"));

    // Missing question
    {
        nlohmann::json args = nlohmann::json::parse(R"({"options":["a"]})");
        auto res = tool.run(args);
        CHECK(!res.has_value());
        CHECK(res.error().msg.find("question") != std::string::npos);
    }

    // Empty question
    {
        nlohmann::json args =
            nlohmann::json::parse(R"({"question":"","options":["a"]})");
        auto res = tool.run(args);
        CHECK(!res.has_value());
        CHECK(res.error().msg.find("must not be empty") != std::string::npos);
    }

    // Missing options
    {
        nlohmann::json args = nlohmann::json::parse(R"({"question":"q"})");
        auto res = tool.run(args);
        CHECK(!res.has_value());
        CHECK(res.error().msg.find("options") != std::string::npos);
    }

    // Empty options array
    {
        nlohmann::json args =
            nlohmann::json::parse(R"({"question":"q","options":[]})");
        auto res = tool.run(args);
        CHECK(!res.has_value());
        CHECK(res.error().msg.find("must not be empty") != std::string::npos);
    }

    // Non-string option
    {
        nlohmann::json args =
            nlohmann::json::parse(R"({"question":"q","options":[42]})");
        auto res = tool.run(args);
        CHECK(!res.has_value());
        CHECK(res.error().msg.find("only strings") != std::string::npos);
    }
}

// --- ask_user through gate (integration) ----------------------------------

TEST("ask_user: through gate returns answer") {
    QuestionGate gate;
    auto tool = ask_user_tool(&gate);

    std::optional<std::string> result;
    std::thread worker([&] {
        nlohmann::json args = nlohmann::json::parse(
            R"({"question":"Color?","options":["Red","Green","Blue"]})");
        auto res = tool.run(args);
        if (res) result = *res;
    });

    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(gate.pending());
    gate.answer("Green");
    worker.join();
    CHECK(result.has_value());
    CHECK_EQ(*result, std::string("Green"));
}

TEST("ask_user: halt through gate returns USER_HALTED") {
    QuestionGate gate;
    auto tool = ask_user_tool(&gate);

    std::optional<std::string> result;
    std::thread worker([&] {
        nlohmann::json args =
            nlohmann::json::parse(R"({"question":"q","options":["a"]})");
        auto res = tool.run(args);
        if (res) result = *res;
    });

    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(gate.pending());
    gate.halt();
    worker.join();
    CHECK(result.has_value());
    CHECK_EQ(*result, std::string("USER_HALTED"));
}

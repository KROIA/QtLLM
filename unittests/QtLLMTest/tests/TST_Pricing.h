#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>


// Tests for the pricing system: ModelPricing derivation, PricingRegistry
// resolution layers (resolver > injected > loaded > builtin), LiteLLM JSON
// parsing and cost estimation.
class TST_Pricing : public UnitTest::Test
{
    TEST_CLASS(TST_Pricing)
public:
    TST_Pricing()
        : Test("TST_Pricing")
    {
        ADD_TEST(TST_Pricing::testCacheDerivation);
        ADD_TEST(TST_Pricing::testBuiltinFallback);
        ADD_TEST(TST_Pricing::testEstimateCost);
        ADD_TEST(TST_Pricing::testLoadLiteLlmJson);
        ADD_TEST(TST_Pricing::testInjectedOverridesLoaded);
        ADD_TEST(TST_Pricing::testResolverHasHighestPriority);
        ADD_TEST(TST_Pricing::testProviderPrefixNormalization);
    }

private:

    // The registry is a process-wide singleton — reset before each test.
    static QtLLM::PricingRegistry& cleanRegistry()
    {
        QtLLM::PricingRegistry& reg = QtLLM::PricingRegistry::instance();
        reg.setResolver(nullptr);
        reg.clearCustomPricing();
        return reg;
    }


    TEST_FUNCTION(testCacheDerivation)
    {
        TEST_START;

        QtLLM::ModelPricing p{10.0, 50.0, -1.0, -1.0};
        TEST_ASSERT(p.isValid());
        TEST_COMPARE(p.effectiveCacheRead(), 1.0);    // 10% of input
        TEST_COMPARE(p.effectiveCacheWrite(), 12.5);  // 125% of input

        QtLLM::ModelPricing explicitCache{10.0, 50.0, 2.0, 20.0};
        TEST_COMPARE(explicitCache.effectiveCacheRead(), 2.0);
        TEST_COMPARE(explicitCache.effectiveCacheWrite(), 20.0);

        TEST_ASSERT(!QtLLM::ModelPricing().isValid());
    }


    TEST_FUNCTION(testBuiltinFallback)
    {
        TEST_START;

        QtLLM::PricingRegistry& reg = cleanRegistry();
        QtLLM::ModelPricing p = reg.pricingFor("claude-haiku-3-something");
        TEST_COMPARE(p.inputPerMTok, 0.25);
        TEST_COMPARE(p.outputPerMTok, 1.25);

        TEST_ASSERT(!reg.pricingFor("totally-unknown-model").isValid());
    }


    TEST_FUNCTION(testEstimateCost)
    {
        TEST_START;

        QtLLM::PricingRegistry& reg = cleanRegistry();
        reg.setPricing("testmodel", {10.0, 50.0, 1.0, 12.5});

        // 1M of each category: 10 + 50 + 1 + 12.5
        double cost = reg.estimateCostUsd("testmodel",
                                          1'000'000, 1'000'000,
                                          1'000'000, 1'000'000);
        TEST_COMPARE(cost, 73.5);

        TEST_COMPARE(reg.estimateCostUsd("unknown-model", 1'000'000, 1'000'000), 0.0);
        cleanRegistry();
    }


    TEST_FUNCTION(testLoadLiteLlmJson)
    {
        TEST_START;

        QtLLM::PricingRegistry& reg = cleanRegistry();
        QJsonObject root{
            {"claude-example-9", QJsonObject{
                {"input_cost_per_token", 0.000003},
                {"output_cost_per_token", 0.000015},
                {"cache_read_input_token_cost", 0.0000003},
                {"cache_creation_input_token_cost", 0.00000375},
                {"litellm_provider", "anthropic"}}},
            {"free-model", QJsonObject{
                {"input_cost_per_token", 0.0},
                {"output_cost_per_token", 0.0}}},
            {"sample_spec", QJsonObject{{"comment", "not a model"}}}};

        TEST_COMPARE(reg.loadLiteLlmJson(root), 1);   // only the paid model

        QtLLM::ModelPricing p = reg.pricingFor("claude-example-9");
        TEST_COMPARE(p.inputPerMTok, 3.0);
        TEST_COMPARE(p.outputPerMTok, 15.0);
        TEST_COMPARE(p.effectiveCacheRead(), 0.3);
        TEST_COMPARE(p.effectiveCacheWrite(), 3.75);

        // Date-suffixed key must still match the shorter app-side model id
        TEST_ASSERT(reg.pricingFor("claude-example-9-20990101").isValid());
        cleanRegistry();
    }


    TEST_FUNCTION(testInjectedOverridesLoaded)
    {
        TEST_START;

        QtLLM::PricingRegistry& reg = cleanRegistry();
        reg.loadLiteLlmJson(QJsonObject{
            {"mymodel", QJsonObject{{"input_cost_per_token", 0.000001},
                                    {"output_cost_per_token", 0.000002}}}});
        reg.setPricing("mymodel", {99.0, 100.0});

        TEST_COMPARE(reg.pricingFor("mymodel").inputPerMTok, 99.0);
        cleanRegistry();
    }


    TEST_FUNCTION(testResolverHasHighestPriority)
    {
        TEST_START;

        QtLLM::PricingRegistry& reg = cleanRegistry();
        reg.setPricing("mymodel", {99.0, 100.0});
        reg.setResolver([](const QString& model) -> QtLLM::ModelPricing {
            if (model == "mymodel")
                return {1.0, 2.0};
            return {};   // invalid -> fall through to the other layers
        });

        TEST_COMPARE(reg.pricingFor("mymodel").inputPerMTok, 1.0);
        // Resolver declines -> injected entry unaffected for other models,
        // builtin fallback still reachable
        TEST_COMPARE(reg.pricingFor("claude-haiku-3").inputPerMTok, 0.25);

        reg.setResolver(nullptr);
        TEST_COMPARE(reg.pricingFor("mymodel").inputPerMTok, 99.0);
        cleanRegistry();
    }


    TEST_FUNCTION(testProviderPrefixNormalization)
    {
        TEST_START;

        QtLLM::PricingRegistry& reg = cleanRegistry();
        reg.loadLiteLlmJson(QJsonObject{
            {"anthropic/claude-special", QJsonObject{
                {"input_cost_per_token", 0.000005},
                {"output_cost_per_token", 0.000010}}}});

        // Both spellings resolve to the same entry
        TEST_COMPARE(reg.pricingFor("claude-special").inputPerMTok, 5.0);
        TEST_COMPARE(reg.pricingFor("Anthropic/Claude-Special").inputPerMTok, 5.0);
        cleanRegistry();
    }
};

TEST_INSTANTIATE(TST_Pricing);

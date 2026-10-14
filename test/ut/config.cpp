#include <gc/config.hpp>

#include <cstdlib>
#include <iostream>

#include "test_util.hpp"

namespace
{

void unset_config_env()
{
    unsetenv("STELLA_GC_SCANS_PER_STEP");
    unsetenv("STELLA_GC_COLLECTION_THRESHOLD");
}

void defaults_without_environment()
{
    unset_config_env();

    auto const cfg = gc::config::fromEnvironment();

    CHECK(cfg.abi_version == STELLA_C_GC_ABI_VERSION);
    CHECK(cfg.max_heap_bytes == 0);
    CHECK(cfg.scansPerStep == 1);
    CHECK(cfg.collectionThreshhold == 100);
}

void environment_overrides_fields()
{
    unset_config_env();
    setenv("STELLA_GC_SCANS_PER_STEP", "4", 1);
    setenv("STELLA_GC_COLLECTION_THRESHOLD", "75", 1);

    auto const cfg = gc::config::fromEnvironment();

    CHECK(cfg.abi_version == STELLA_C_GC_ABI_VERSION);
    CHECK(cfg.max_heap_bytes == 0);
    CHECK(cfg.scansPerStep == 4);
    CHECK(cfg.collectionThreshhold == 75);

    unset_config_env();
}

void unset_variables_are_ignored()
{
    unset_config_env();
    setenv("STELLA_GC_SCANS_PER_STEP", "7", 1);

    auto const cfg = gc::config::fromEnvironment();

    CHECK(cfg.max_heap_bytes == 0);
    CHECK(cfg.scansPerStep == 7);
    CHECK(cfg.collectionThreshhold == 100);

    unset_config_env();
}

void conversion_from_stella_config()
{
    StellaGcConfig base = test::make_config(4096);
    gc::config cfg{base};

    CHECK(cfg.abi_version == base.abi_version);
    CHECK(cfg.max_heap_bytes == base.max_heap_bytes);
    CHECK(cfg.scansPerStep == 1);
    CHECK(cfg.collectionThreshhold == 100);
}

void environment_never_overrides_heap_bytes()
{
    unset_config_env();
    setenv("STELLA_GC_SCANS_PER_STEP", "2", 1);

    StellaGcConfig base = test::make_config(4096);
    gc::config cfg{base};
    cfg.readEnvs();

    CHECK(cfg.max_heap_bytes == 4096);
    CHECK(cfg.scansPerStep == 2);
    CHECK(cfg.collectionThreshhold == 100);

    unset_config_env();
}

void invalid_environment_is_violation()
{
    unset_config_env();
    setenv("STELLA_GC_SCANS_PER_STEP", "abc", 1);
    CHECK_VIOLATION(gc::config::fromEnvironment();, "invalid value of STELLA_GC_SCANS_PER_STEP environment variable");

    unset_config_env();
    setenv("STELLA_GC_COLLECTION_THRESHOLD", "75%", 1);
    CHECK_VIOLATION(gc::config::fromEnvironment();, "invalid value of STELLA_GC_COLLECTION_THRESHOLD environment variable");

    unset_config_env();
}

void run_all()
{
    defaults_without_environment();
    environment_overrides_fields();
    unset_variables_are_ignored();
    conversion_from_stella_config();
    environment_never_overrides_heap_bytes();
    invalid_environment_is_violation();
}

} // namespace

int main()
{
    run_all();

    std::cout << test::checks() << " checks, " << test::failures() << " failures\n";
    return test::failures() == 0 ? 0 : 1;
}
#include "Config.h"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Config config;
    if (config.mysql_enabled || config.mysql_command_recovery_enabled ||
        config.dispatchPersistenceConfig().command_recovery_enabled) {
        std::cerr << "MySQL and command recovery must default to false\n";
        return 1;
    }
    if (!config.loadYaml(argv[1]) ||
        !config.mysql_command_recovery_enabled ||
        !config.dispatchPersistenceConfig().command_recovery_enabled ||
        config.lineRouteAnalyzerConfig().intersection_min_angle_degrees != 12.0) {
        std::cerr << "YAML value did not reach its runtime config\n";
        return 1;
    }
    std::cout << "CONFIG_COMMAND_RECOVERY_OK\n";
    return 0;
}

#include "nordend/core/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

#include "nordend/core/command_line.hpp"
#include "nordend/core/file.hpp"

namespace nordend::core {
namespace {

const char* type_name(const toml::node& node) {
    switch (node.type()) {
        case toml::node_type::string:
            return "a string";
        case toml::node_type::integer:
            return "an integer";
        case toml::node_type::floating_point:
            return "a number with decimals";
        case toml::node_type::boolean:
            return "a boolean";
        case toml::node_type::date:
            return "a date";
        case toml::node_type::time:
            return "a time";
        case toml::node_type::date_time:
            return "a date-time";
        case toml::node_type::array:
            return "an array";
        case toml::node_type::table:
            return "a table";
        case toml::node_type::none:
            break;
    }
    return "nothing";
}

// Walks the parsed document, filling the config and collecting problems.
class Reader {
public:
    // A key inside a section and how to read its value.
    struct Field {
        std::string_view name;
        std::function<void(const toml::node& node, const std::string& path)> read;
    };

    explicit Reader(std::string_view source) : source_{source} {}

    void report(const toml::source_region& where, std::string_view what) {
        problems_.push_back({where.begin, std::format("{}:{}:{}: {}", source_, where.begin.line,
                                                      where.begin.column, what)});
    }

    [[nodiscard]] std::size_t problem_count() const noexcept { return problems_.size(); }

    // A problem said right after the last of those reported since problem_count() was since.
    void report_after(std::size_t since, std::string_view what) {
        toml::source_position where{};
        for (std::size_t i = since; i < problems_.size(); ++i) {
            if (std::pair{problems_[i].where.line, problems_[i].where.column} >=
                std::pair{where.line, where.column}) {
                where = problems_[i].where;
            }
        }
        problems_.push_back(
            {where, std::format("{}:{}:{}: {}", source_, where.line, where.column, what)});
    }

    // Problems in file order (toml++ tables iterate by key, not by position).
    [[nodiscard]] std::vector<std::string> take_problems() {
        std::ranges::stable_sort(problems_, [](const Problem& a, const Problem& b) {
            return std::pair{a.where.line, a.where.column} <
                   std::pair{b.where.line, b.where.column};
        });
        std::vector<std::string> out;
        out.reserve(problems_.size());
        for (Problem& problem : problems_) {
            out.push_back(std::move(problem.text));
        }
        problems_.clear();
        return out;
    }

    void read_section(const toml::node& node, std::string_view section,
                      std::initializer_list<Field> fields) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            report(node.source(), std::format("'{}' must be a section ([{}]), got {}; ignored",
                                              section, section, type_name(node)));
            return;
        }
        for (const auto& [key, value] : *table) {
            const std::string path = std::format("{}.{}", section, key.str());
            bool known = false;
            for (const Field& field : fields) {
                if (key.str() == field.name) {
                    field.read(value, path);
                    known = true;
                    break;
                }
            }
            if (!known) {
                report(key.source(), std::format("unknown key '{}'; ignored", path));
            }
        }
    }

    void read_string(const toml::node& node, const std::string& path, std::string& out) {
        const auto* value = node.as_string();
        if (value == nullptr) {
            report(node.source(), std::format("'{}' must be a string, got {}; using \"{}\"", path,
                                              type_name(node), out));
            return;
        }
        if (value->get().empty()) {
            report(node.source(), std::format("'{}' must not be empty; using \"{}\"", path, out));
            return;
        }
        out = value->get();
    }

    // A list of strings; anything else keeps out.
    void read_strings(const toml::node& node, const std::string& path,
                      std::vector<std::string>& out) {
        const toml::array* array = node.as_array();
        if (array == nullptr) {
            report(node.source(), std::format("'{}' must be a list of strings, got {}; ignored",
                                              path, type_name(node)));
            return;
        }
        std::vector<std::string> strings;
        for (const toml::node& item : *array) {
            const auto* value = item.as_string();
            if (value == nullptr) {
                report(item.source(),
                       std::format("'{}' must be a list of strings, got {} in it; ignored", path,
                                   type_name(item)));
                return;
            }
            strings.push_back(value->get());
        }
        out = std::move(strings);
    }

    void read_bool(const toml::node& node, const std::string& path, bool& out) {
        const auto* value = node.as_boolean();
        if (value == nullptr) {
            report(node.source(), std::format("'{}' must be true or false, got {}; using {}", path,
                                              type_name(node), out));
            return;
        }
        out = value->get();
    }

    void read_opacity(const toml::node& node, const std::string& path, float& out) {
        const auto value = node.value<double>();
        if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 1.0) {
            report(node.source(),
                   std::format("'{}' must be a number between 0 and 1; using {}", path, out));
            return;
        }
        out = static_cast<float>(*value);
    }

    // An integer or a decimal number between min and max.
    void read_number(const toml::node& node, const std::string& path, double min, double max,
                     float& out) {
        const auto value = node.value<double>();
        if (!value || !std::isfinite(*value)) {
            report(node.source(), std::format("'{}' must be a number, got {}; using {:g}", path,
                                              type_name(node), out));
            return;
        }
        if (*value < min || *value > max) {
            report(node.source(),
                   std::format("'{}' must be between {:g} and {:g}, got {:g}; using {:g}", path,
                               min, max, *value, out));
            return;
        }
        out = static_cast<float>(*value);
    }

    void read_int(const toml::node& node, const std::string& path, int min, int max, int& out) {
        const auto* value = node.as_integer();
        if (value == nullptr) {
            report(node.source(), std::format("'{}' must be an integer, got {}; using {}", path,
                                              type_name(node), out));
            return;
        }
        const std::int64_t number = value->get();
        if (number < min || number > max) {
            report(node.source(), std::format("'{}' must be between {} and {}, got {}; using {}",
                                              path, min, max, number, out));
            return;
        }
        out = static_cast<int>(number);
    }

private:
    struct Problem {
        toml::source_position where;
        std::string text;
    };

    std::string_view source_;
    std::vector<Problem> problems_;
};

// The field codes an action's command may hold.
constexpr std::string_view kActionCodes{"fFuUd%"};

// An action's command line: a string split as an Exec value, or an array of words.
void read_action_exec(const toml::node& node, const std::string& path, Reader& reader,
                      Config::Action& action) {
    if (const auto* line = node.as_string()) {
        const std::optional<std::vector<std::string>> words = split_command_line(line->get());
        if (!words) {
            reader.report(node.source(),
                          std::format("'{}' cannot be split into words (a quote not closed, or "
                                      "nothing in it)",
                                      path));
            return;
        }
        // Every code inside a word is one: each must be known, and the paths come one way.
        bool list = false;
        bool one = false;
        for (const std::string& word : *words) {
            for (std::size_t i = 0; i < word.size(); ++i) {
                if (word[i] != '%') {
                    continue;
                }
                const char code = i + 1 < word.size() ? word[i + 1] : '\0';
                if (kActionCodes.find(code) == std::string_view::npos || code == '\0') {
                    reader.report(
                        node.source(),
                        std::format("'{}' has an unknown field code '%{}' (known: %f "
                                    "%F %u %U %d %%)",
                                    path, code == '\0' ? std::string{} : std::string{code}));
                    return;
                }
                list = list || code == 'F' || code == 'U';
                one = one || code == 'f' || code == 'u';
                ++i;
            }
        }
        if (list && one) {
            reader.report(node.source(),
                          std::format("'{}' takes the paths both as a list (%F %U) and one at a "
                                      "time (%f %u): choose one",
                                      path));
            return;
        }
        action.exec = *words;
        action.exec_words = false;
        return;
    }
    const toml::array* array = node.as_array();
    if (array == nullptr) {
        reader.report(node.source(), std::format("'{}' must be a string or a list of strings, "
                                                 "got {}",
                                                 path, type_name(node)));
        return;
    }
    std::vector<std::string> words;
    bool list = false;
    bool one = false;
    for (const toml::node& item : *array) {
        const auto* word = item.as_string();
        if (word == nullptr) {
            reader.report(item.source(), std::format("'{}' must be a list of strings, got {} in it",
                                                     path, type_name(item)));
            return;
        }
        const std::string& text = word->get();
        if (text.size() == 2 && text[0] == '%') {  // only a whole word is a code
            if (kActionCodes.find(text[1]) == std::string_view::npos) {
                reader.report(item.source(),
                              std::format("'{}' has an unknown field code '{}' (known: %f %F %u "
                                          "%U %d %%)",
                                          path, text));
                return;
            }
            list = list || text[1] == 'F' || text[1] == 'U';
            one = one || text[1] == 'f' || text[1] == 'u';
        }
        words.push_back(text);
    }
    if (words.empty() || words.front().empty()) {
        reader.report(node.source(), std::format("'{}' must start with a program", path));
        return;
    }
    if (list && one) {
        reader.report(node.source(),
                      std::format("'{}' takes the paths both as a list (%F %U) and one at a time "
                                  "(%f %u): choose one",
                                  path));
        return;
    }
    action.exec = std::move(words);
    action.exec_words = true;
}

// One [[action]] table; false when it has a problem (it is then left out: a filter misspelt must
// not offer a command for files it was not meant for).
bool read_action(const toml::node& node, std::size_t number, Reader& reader, Config& config) {
    const std::string section = std::format("action[{}]", number);
    const std::size_t before = reader.problem_count();
    Config::Action action;
    bool named = false;
    bool has_exec = false;
    reader.read_section(
        node, section,
        {{"name",
          [&](const toml::node& value, const std::string& path) {
              named = true;
              reader.read_string(value, path, action.name);
          }},
         {"exec",
          [&](const toml::node& value, const std::string& path) {
              has_exec = true;
              read_action_exec(value, path, reader, action);
          }},
         {"kinds",
          [&](const toml::node& value, const std::string& path) {
              std::vector<std::string> kinds;
              reader.read_strings(value, path, kinds);
              if (reader.problem_count() > before) {
                  return;
              }
              if (kinds.empty()) {
                  reader.report(value.source(),
                                std::format(R"('{}' must name "file", "folder" or both)", path));
                  return;
              }
              action.files = false;
              action.folders = false;
              for (const std::string& kind : kinds) {
                  if (kind == "file") {
                      action.files = true;
                  } else if (kind == "folder") {
                      action.folders = true;
                  } else {
                      reader.report(
                          value.source(),
                          std::format(R"('{}' takes "file" and "folder", got "{}")", path, kind));
                  }
              }
          }},
         {"patterns",
          [&](const toml::node& value, const std::string& path) {
              reader.read_strings(value, path, action.patterns);
          }},
         {"mime",
          [&](const toml::node& value, const std::string& path) {
              reader.read_strings(value, path, action.mime);
              for (const std::string& type : action.mime) {
                  if (type.find('*') != std::string::npos) {
                      reader.report(value.source(),
                                    std::format("'{}': wildcards (\"{}\") are not supported; "
                                                "name the types",
                                                path, type));
                  } else if (type.find('/') == std::string::npos) {
                      reader.report(
                          value.source(),
                          std::format(R"('{}': "{}" is not a type ("image/png"))", path, type));
                  }
              }
          }},
         {"min",
          [&](const toml::node& value, const std::string& path) {
              reader.read_int(value, path, 1, 1'000'000, action.min);
          }},
         {"max",
          [&](const toml::node& value, const std::string& path) {
              reader.read_int(value, path, 1, 1'000'000, action.max);
          }},
         {"background",
          [&](const toml::node& value, const std::string& path) {
              reader.read_bool(value, path, action.background);
          }},
         {"submenu",
          [&](const toml::node& value, const std::string& path) {
              reader.read_string(value, path, action.submenu);
          }},
         {"terminal", [&](const toml::node& value, const std::string& path) {
              reader.read_bool(value, path, action.terminal);
          }}});
    if (node.is_table()) {
        if (!named) {
            reader.report(node.source(), std::format("'{}' needs a name", section));
        }
        if (!has_exec) {
            reader.report(node.source(), std::format("'{}' needs an exec", section));
        }
        if (action.max != 0 && action.max < action.min) {
            reader.report(node.source(), std::format("'{}': max ({}) is less than min ({})",
                                                     section, action.max, action.min));
        }
    }
    if (reader.problem_count() > before) {
        // Said after what is wrong in it.
        reader.report_after(
            before, std::format("'{}'{} is left out", section,
                                action.name.empty() ? "" : std::format(" (\"{}\")", action.name)));
        return false;
    }
    config.actions.push_back(std::move(action));
    return true;
}

void read_document(const toml::table& root, Reader& reader, Config& config) {
    for (const auto& [key, node] : root) {
        if (key.str() == "font") {
            reader.read_section(node, "font",
                                {{"family",
                                  [&](const toml::node& value, const std::string& path) {
                                      reader.read_string(value, path, config.font.family);
                                  }},
                                 {"monospace",
                                  [&](const toml::node& value, const std::string& path) {
                                      reader.read_string(value, path, config.font.monospace);
                                  }},
                                 {"size", [&](const toml::node& value, const std::string& path) {
                                      reader.read_number(value, path, 8.0, 32.0, config.font.size);
                                  }}});
        } else if (key.str() == "pointer") {
            reader.read_section(
                node, "pointer",
                {{"double_click_ms",
                  [&](const toml::node& value, const std::string& path) {
                      reader.read_int(value, path, 100, 2000, config.pointer.double_click_ms);
                  }},
                 {"double_click_distance", [&](const toml::node& value, const std::string& path) {
                      reader.read_int(value, path, 0, 50, config.pointer.double_click_distance);
                  }}});
        } else if (key.str() == "window") {
            reader.read_section(
                node, "window",
                {{"title_bar",
                  [&](const toml::node& value, const std::string& path) {
                      std::string choice{"integrated"};  // also what a wrong type keeps
                      reader.read_string(value, path, choice);
                      if (choice == "integrated") {
                          config.window.title_bar = Config::Window::TitleBar::integrated;
                      } else if (choice == "compositor") {
                          config.window.title_bar = Config::Window::TitleBar::compositor;
                      } else {
                          reader.report(value.source(),
                                        std::format("'{}' must be \"integrated\" or "
                                                    "\"compositor\"; using \"integrated\"",
                                                    path));
                      }
                  }},
                 {"background_opacity",
                  [&](const toml::node& value, const std::string& path) {
                      reader.read_opacity(value, path, config.window.background_opacity);
                  }},
                 {"corner_radius", [&](const toml::node& value, const std::string& path) {
                      reader.read_int(value, path, 0, 24, config.window.corner_radius);
                  }}});
        } else if (key.str() == "terminal") {
            reader.read_section(node, "terminal",
                                {{"program",
                                  [&](const toml::node& value, const std::string& path) {
                                      reader.read_string(value, path, config.terminal.program);
                                  }},
                                 {"args", [&](const toml::node& value, const std::string& path) {
                                      reader.read_strings(value, path, config.terminal.args);
                                  }}});
        } else if (key.str() == "open") {
            reader.read_section(
                node, "open", {{"programs", [&](const toml::node& value, const std::string& path) {
                                    reader.read_bool(value, path, config.open.programs);
                                }}});
        } else if (key.str() == "sidebar") {
            reader.read_section(node, "sidebar",
                                {{"tree", [&](const toml::node& value, const std::string& path) {
                                      reader.read_bool(value, path, config.sidebar.tree);
                                  }}});
        } else if (key.str() == "action") {
            const toml::array* actions = node.as_array();
            if (actions == nullptr) {
                reader.report(node.source(),
                              std::format("'action' must be a list of tables ([[action]]), got {}; "
                                          "ignored",
                                          type_name(node)));
                continue;
            }
            std::size_t number = 0;
            for (const toml::node& action : *actions) {
                read_action(action, ++number, reader, config);
            }
        } else if (key.str() == "icon") {
            reader.read_section(
                node, "icon",
                {{"follow_accent", [&](const toml::node& value, const std::string& path) {
                      reader.read_bool(value, path, config.icon.follow_accent);
                  }}});
        } else {
            reader.report(key.source(),
                          std::format("unknown section or key '{}'; ignored", key.str()));
        }
    }
}

}  // namespace

std::filesystem::path default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
    // The XDG base directory spec ignores relative paths.
    if (xdg != nullptr && xdg[0] == '/') {
        return std::filesystem::path{xdg} / "nordend" / "config.toml";
    }
    const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".config" / "nordend" / "config.toml";
    }
    return {};
}

ConfigResult parse_config(std::string_view text, std::string_view source) {
    ConfigResult result;
    Reader reader{source};
    toml::table root;
    try {
        root = toml::parse(text, source);
        result.from_file = true;
        read_document(root, reader, result.config);
    } catch (const toml::parse_error& e) {
        reader.report(e.source(), std::format("{}; the whole file is ignored, using defaults",
                                              e.description()));
    }
    result.problems = reader.take_problems();
    return result;
}

ConfigResult load_config(const std::filesystem::path& path) {
    const TextFile file = read_text_file(path);
    if (!file.exists) {
        return {};
    }
    if (!file.error.empty()) {
        ConfigResult result;
        result.problems.push_back(std::format("{}: {}; using defaults", path.string(), file.error));
        return result;
    }
    return parse_config(file.text, path.string());
}

}  // namespace nordend::core

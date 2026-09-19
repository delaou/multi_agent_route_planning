// Diagnostic counterexamples, not acceptance tests. Run against the current analyzer.
#include "LineRouteAnalyzer.h"
#include <iostream>
LineRoute line(const char* id, std::initializer_list<LineRoutePoint> points) {
    LineRoute r; r.line_id=id; r.points=points; return r;
}
void show(const char* name, const std::vector<LineRoute>& routes) {
    auto result=LineRouteAnalyzer{}.analyze(routes);
    std::cout << name << " count=" << result.shared_corridors.size() << '\n';
    for (const auto& c:result.shared_corridors) {
        std::cout << "  " << c.start_x << " -> " << c.end_x << " length=" << c.length;
        for (const auto& m:c.members) std::cout << ' ' << m.line_id << ':' << m.start_segment << '-' << m.end_segment;
        std::cout << '\n';
    }
}
int main() {
    show("partial_three_members", {
        line("A", {{"",0,0,0},{"",300,0,0}}),
        line("B", {{"",0,0,0},{"",300,0,0}}),
        line("C", {{"",100,0,0},{"",200,0,0}})});
    show("repeated_short_fragments", {
        line("A", {{"",0,0,0},{"",20,0,0},{"",40,0,0},{"",60,0,0},{"",80,0,0},{"",100,0,0}}),
        line("B", {{"",0,0,0},{"",20,0,0},{"",40,0,0},{"",60,0,0},{"",80,0,0},{"",100,0,0},
                   {"",500,0,500},{"",0,0,0},{"",20,0,0},{"",40,0,0},{"",60,0,0},{"",80,0,0},{"",100,0,0}})});
    show("parallel_tracks_four_metres_apart", {
        line("A", {{"",0,0,0},{"",100,0,0}}),
        line("B", {{"",0,0,4},{"",100,0,4}})});
    show("single_line_out_and_back", {
        line("A", {{"",0,0,0},{"",100,0,0},{"",0,0,0}})});
    show("member_change_merge", {
        line("A", {{"",0,0,0},{"",40,0,0},{"",80,0,0}}),
        line("B", {{"",0,0,0},{"",40,0,0},{"",80,0,0},
                   {"",500,0,500},{"",0,0,0},{"",40,0,0}})});
}

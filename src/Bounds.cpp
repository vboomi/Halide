#include "Bounds.h"
#include "IRVisitor.h"
#include "IR.h"
#include "IROperator.h"
#include "IREquality.h"
#include "Simplify.h"
#include "IRPrinter.h"
#include "Util.h"
#include "Var.h"
#include "Debug.h"
#include "CSE.h"
#include "Derivative.h"
#include <iostream>

namespace Halide {
namespace Internal {

using std::make_pair;
using std::map;
using std::vector;
using std::string;

class Bounds : public IRVisitor {
public:
    Expr min, max;
    const Scope<Interval> &scope;
    Scope<Interval> inner_scope;

    Bounds(const Scope<Interval> &s) : scope(s) {}
private:
    void bounds_of_type(Type t) {
        if (t.is_uint() && t.bits <= 16) {
            max = cast(t, (1 << t.bits) - 1);
            min = cast(t, 0);
        } else if (t.is_int() && t.bits <= 16) {
            max = cast(t, (1 << (t.bits-1)) - 1);
            min = cast(t, -(1 << (t.bits-1)));
        } else {
            max = Expr();
            min = Expr();
        }
    }

    using IRVisitor::visit;

    void visit(const IntImm *op) {
        min = op;
        max = op;
    }

    void visit(const FloatImm *op) {
        min = op;
        max = op;
    }

    void visit(const Cast *op) {

        op->value.accept(this);
        Expr min_a = min, max_a = max;

        if (min_a.same_as(op->value) && max_a.same_as(op->value)) {
            min = max = op;
            return;
        }

        Type to = op->type;
        Type from = op->value.type();

        if (min_a.defined() && min_a.same_as(max_a)) {
            min = max = Cast::make(to, min_a);
            return;
        }

        // If overflow is impossible, cast the min and max. If it's
        // possible, use the bounds of the destination type.
        bool could_overflow = true;
        if (to.is_float()) {
            could_overflow = false;
        } else if (to.is_int() && from.is_int() && to.bits >= from.bits) {
            could_overflow = false;
        } else if (to.is_uint() && from.is_uint() && to.bits >= from.bits) {
            could_overflow = false;
        } else if (to.is_int() && from.is_uint() && to.bits > from.bits) {
            could_overflow = false;
        } else if (to.is_int() && to.bits >= 32) {
            // Warning: dubious code ahead.

            // If we cast to an int32 or greater, assume that it won't
            // overflow. Otherwise expressions like
            // cast<int32_t>(bounded_float) barf.
            could_overflow = false;
        }

        // If min and max are different constants that fit into the
        // narrower type, we should allow it.
        if (from == Int(32) && min_a.defined() && max_a.defined()) {
            if (const IntImm *min_int = min_a.as<IntImm>()) {
                if (const IntImm *max_int = max_a.as<IntImm>()) {
                    if (to.is_uint() && to.bits <= 32 &&
                        min_int->value >= 0 &&
                        (to.bits == 32 || (max_int->value < (1 << to.bits)))) {
                        could_overflow = false;
                    } else if (to.is_int() && to.bits <= 32 &&
                               min_int->value >= -(1 << (to.bits-1)) &&
                               max_int->value < (1 << (to.bits-1))) {
                        could_overflow = false;
                    }
                }
            }
        }

        if (from == Float(32) && min_a.defined() && max_a.defined()) {
            if (const FloatImm *min_float = min_a.as<FloatImm>()) {
                if (const FloatImm *max_float = max_a.as<FloatImm>()) {
                    double max_magnitude = ::pow(2.0, to.bits-1);
                    if (to.is_uint() &&
                        min_float->value >= 0.0f &&
                        max_float->value < 2.0*max_magnitude) {
                        could_overflow = false;
                    } else if (to.is_int() &&
                               min_float->value >= -max_magnitude &&
                               max_float->value < max_magnitude) {
                        could_overflow = false;
                    }
                }
            }
        }

        if (!could_overflow) {
            // Start with the bounds of the narrow type.
            bounds_of_type(from);
            // If we have a better min or max for the arg use that.
            if (min_a.defined()) min = min_a;
            if (max_a.defined()) max = max_a;
            // Then cast those bounds to the wider type.
            if (min.defined()) min = Cast::make(to, min);
            if (max.defined()) max = Cast::make(to, max);
        } else {
            // This might overflow, so use the bounds of the destination type.
            bounds_of_type(to);
        }
    }

    void visit(const Variable *op) {
        if (scope.contains(op->name)) {
            Interval bounds = scope.get(op->name);
            min = bounds.min;
            max = bounds.max;
        } else if (inner_scope.contains(op->name)) {
            Interval bounds = inner_scope.get(op->name);
            min = bounds.min;
            max = bounds.max;
        } else {
            debug(3) << op->name << " not in scope, so leaving it as-is\n";
            min = op;
            max = op;
        }
    }

    void visit(const Add *op) {
        op->a.accept(this);
        Expr min_a = min, max_a = max;
        op->b.accept(this);
        Expr min_b = min, max_b = max;

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        min = (min_b.defined() && min_a.defined()) ? Add::make(min_a, min_b) : Expr();

        if (min_a.same_as(max_a) && min_b.same_as(max_b)) {
            max = min;
        } else {
            max = (max_b.defined() && max_a.defined()) ? Add::make(max_a, max_b) : Expr();
        }

        // Check for overflow for (u)int8 and (u)int16
        if (!op->type.is_float() && op->type.bits < 32) {
            if (max.defined()) {
                Expr test = (cast<int>(max_a) + cast<int>(max_b) == cast<int>(max));
                //debug(0) << "Attempting to prove: " << test << " -> " << simplify(test) << "\n";
                if (!is_one(simplify(test))) {
                    bounds_of_type(op->type);
                    return;
                }
            }
            if (min.defined()) {
                Expr test = (cast<int>(min_a) + cast<int>(min_b) == cast<int>(min));
                //debug(0) << "Attempting to prove: " << test << " -> " << simplify(test) << "\n";
                if (!is_one(simplify(test))) {
                    bounds_of_type(op->type);
                    return;
                }
            }
        }

    }

    void visit(const Sub *op) {
        op->a.accept(this);
        Expr min_a = min, max_a = max;
        op->b.accept(this);
        Expr min_b = min, max_b = max;

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        min = (max_b.defined() && min_a.defined()) ? Sub::make(min_a, max_b) : Expr();
        if (min_a.same_as(max_a) && min_b.same_as(max_b)) {
            max = min;
        } else {
            max = (min_b.defined() && max_a.defined()) ? Sub::make(max_a, min_b) : Expr();
        }

        // Check for overflow for (u)int8 and (u)int16
        if (!op->type.is_float() && op->type.bits < 32) {
            if (max.defined()) {
                Expr test = (cast<int>(max_a) - cast<int>(min_b) == cast<int>(max));
                //debug(0) << "Attempting to prove: " << test << " -> " << simplify(test) << "\n";
                if (!is_one(simplify(test))) {
                    bounds_of_type(op->type);
                    return;
                }
            }
            if (min.defined()) {
                Expr test = (cast<int>(min_a) - cast<int>(max_b) == cast<int>(min));
                //debug(0) << "Attempting to prove: " << test << " -> " << simplify(test) << "\n";
                if (!is_one(simplify(test))) {
                    bounds_of_type(op->type);
                    return;
                }
            }
        }

        // Check underflow for uint
        if (op->type.is_uint()) {
            if (min.defined()) {
                Expr test = (max_b <= min_a);
                if (!is_one(simplify(test))) {
                    bounds_of_type(op->type);
                    return;
                }
            }
        }
    }

    void visit(const Mul *op) {
        op->a.accept(this);
        Expr min_a = min, max_a = max;
        if (!min_a.defined() || !max_a.defined()) {
            min = Expr(); max = Expr(); return;
        }

        op->b.accept(this);
        Expr min_b = min, max_b = max;
        if (!min_b.defined() || !max_b.defined()) {
            min = Expr(); max = Expr(); return;
        }

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        if (min_a.same_as(max_a) && min_b.same_as(max_b)) {
            // A and B are constant
            min = max = min_a * min_b;
        } else if (min_a.same_as(max_a)) {
            // A is constant
            if (is_zero(min_a)) {
                min = max = min_a;
            } else if (is_positive_const(min_a) || op->type.is_uint()) {
                min = min_b * min_a;
                max = max_b * min_a;
            } else if (is_negative_const(min_a)) {
                min = max_b * min_a;
                max = min_b * min_a;
            } else {
                // Sign of a is unknown
                Expr a = min_a * min_b;
                Expr b = min_a * max_b;
                Expr cmp = min_a >= make_zero(min_a.type());
                min = select(cmp, a, b);
                max = select(cmp, b, a);
            }
        } else if (min_b.same_as(max_b)) {
            // B is constant
            if (is_zero(min_b)) {
                min = max = min_a;
            } else if (is_positive_const(min_b) || op->type.is_uint()) {
                min = min_a * min_b;
                max = max_a * min_b;
            } else if (is_negative_const(min_b)) {
                min = max_a * min_b;
                max = min_a * min_b;
            } else {
                // Sign of b is unknown
                Expr a = min_b * min_a;
                Expr b = min_b * max_a;
                Expr cmp = min_b >= make_zero(min_b.type());
                min = select(cmp, a, b);
                max = select(cmp, b, a);
            }
        } else {

            Expr a = min_a * min_b;
            Expr b = min_a * max_b;
            Expr c = max_a * min_b;
            Expr d = max_a * max_b;

            min = Min::make(Min::make(a, b), Min::make(c, d));
            max = Max::make(Max::make(a, b), Max::make(c, d));
        }

        if (op->type.bits < 32 && !op->type.is_float()) {
            // Try to prove it can't overflow
            Expr test1 = (cast<int>(min_a) * cast<int>(min_b) == cast<int>(min_a * min_b));
            Expr test2 = (cast<int>(min_a) * cast<int>(max_b) == cast<int>(min_a * max_b));
            Expr test3 = (cast<int>(max_a) * cast<int>(min_b) == cast<int>(max_a * min_b));
            Expr test4 = (cast<int>(max_a) * cast<int>(max_b) == cast<int>(max_a * max_b));
            if (!is_one(simplify(test1 && test2 && test3 && test4))) {
                bounds_of_type(op->type);
                return;
            }
        }

    }

    void visit(const Div *op) {

        op->a.accept(this);
        Expr min_a = min, max_a = max;
        if (!min_a.defined() || !max_a.defined()) {
            min = Expr(); max = Expr(); return;
        }

        op->b.accept(this);
        Expr min_b = min, max_b = max;
        if (!min_b.defined() || !max_b.defined()) {
            min = Expr(); max = Expr(); return;
        }

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        if (min_b.same_as(max_b)) {
            if (is_zero(min_b)) {
                // Divide by zero. Drat.
                min = Expr();
                max = Expr();
            } else if (is_positive_const(min_b) || op->type.is_uint()) {
                min = min_a / min_b;
                max = max_a / min_b;
            } else if (is_negative_const(min_b)) {
                min = max_a / min_b;
                max = min_a / min_b;
            } else {
                // Sign of b is unknown
                Expr a = min_a / min_b;
                Expr b = max_a / max_b;
                Expr cmp = min_b > make_zero(min_b.type());
                min = select(cmp, a, b);
                max = select(cmp, b, a);
            }
        } else {
            // if we can't statically prove that the divisor can't span zero, then we're unbounded
            bool min_is_positive = is_positive_const(min_b) ||
                equal(const_true(), simplify(min_b > make_zero(min_b.type())));
            bool max_is_negative = is_negative_const(max_b) ||
                equal(const_true(), simplify(max_b < make_zero(max_b.type())));
            if (!equal(min_b, max_b) &&
                !min_is_positive &&
                !max_is_negative) {
                min = Expr();
                max = Expr();
                return;
            }

            // Divisor is either strictly positive or strictly
            // negative, so we can just take the extrema.
            Expr a = min_a / min_b;
            Expr b = min_a / max_b;
            Expr c = max_a / min_b;
            Expr d = max_a / max_b;

            min = Min::make(Min::make(a, b), Min::make(c, d));
            max = Max::make(Max::make(a, b), Max::make(c, d));
        }
    }

    void visit(const Mod *op) {
        op->a.accept(this);
        Expr min_a = min, max_a = max;

        op->b.accept(this);
        Expr min_b = min, max_b = max;
        if (!min_b.defined() || !max_b.defined()) {
            min = Expr(); max = Expr(); return;
        }

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        if (min_a.defined() && min_a.same_as(max_a) && min_b.same_as(max_b)) {
            min = max = Mod::make(min_a, min_b);
        } else {
            // Only consider B (so A can be undefined)
            min = make_zero(op->type);
            max = max_b;
            if (!max.type().is_float()) {
                // Integer modulo returns at most one less than the
                // second arg.
                max = max - make_one(op->type);
            }
        }
    }

    void visit(const Min *op) {
        op->a.accept(this);
        Expr min_a = min, max_a = max;
        op->b.accept(this);
        Expr min_b = min, max_b = max;

        debug(3) << "Bounds of " << Expr(op) << "\n";

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        if (min_a.defined() && min_a.same_as(min_b) &&
            max_a.defined() && max_a.same_as(max_b)) {
            min = min_a;
            max = max_a;
            return;
        }

        if (min_a.defined() && min_b.defined()) {
            min = Min::make(min_a, min_b);
        } else {
            min = Expr();
        }

        if (max_a.defined() && max_b.defined()) {
            max = Min::make(max_a, max_b);
        } else {
            max = max_a.defined() ? max_a : max_b;
        }

        debug(3) << min << ", " << max << "\n";
    }


    void visit(const Max *op) {
        op->a.accept(this);
        Expr min_a = min, max_a = max;
        op->b.accept(this);
        Expr min_b = min, max_b = max;

        debug(3) << "Bounds of " << Expr(op) << "\n";

        if (min_a.same_as(op->a) && max_a.same_as(op->a) &&
            min_b.same_as(op->b) && max_b.same_as(op->b)) {
            min = max = op;
            return;
        }

        if (min_a.defined() && min_a.same_as(min_b) &&
            max_a.defined() && max_a.same_as(max_b)) {
            min = min_a;
            max = max_a;
            return;
        }

        if (min_a.defined() && min_b.defined()) {
            min = Max::make(min_a, min_b);
        } else {
            min = min_a.defined() ? min_a : min_b;
        }

        if (max_a.defined() && max_b.defined()) {
            max = Max::make(max_a, max_b);
        } else {
            max = Expr();
        }

        debug(3) << min << ", " << max << "\n";
    }

    void visit(const EQ *) {
        //assert(false && "Bounds of boolean");
        min = Expr();
        max = Expr();
    }

    void visit(const NE *) {
        // assert(false && "Bounds of boolean");
        min = Expr();
        max = Expr();
    }

    void visit(const LT *) {
        // assert(false && "Bounds of boolean");
        min = Expr();
        max = Expr();
    }

    void visit(const LE *) {
        // assert(false && "Bounds of boolean");
        min = Expr();
        max = Expr();
    }

    void visit(const GT *) {
        // assert(false && "Bounds of boolean");
        min = Expr();
        max = Expr();
    }

    void visit(const GE *) {
        // assert(false && "Bounds of comparison");
        min = Expr();
        max = Expr();
    }

    void visit(const And *) {
        // assert(false && "Bounds of comparison");
        min = Expr();
        max = Expr();
    }

    void visit(const Or *) {
        // assert(false && "Bounds of comparison");
        min = Expr();
        max = Expr();
    }

    void visit(const Not *) {
        // assert(false && "Bounds of comparison");
        min = Expr();
        max = Expr();
    }

    void visit(const Select *op) {
        op->true_value.accept(this);
        Expr min_a = min, max_a = max;
        if (!min_a.defined() || !max_a.defined()) {
            min = Expr(); max = Expr(); return;
        }

        op->false_value.accept(this);
        Expr min_b = min, max_b = max;
        if (!min_b.defined() || !max_b.defined()) {
            min = Expr(); max = Expr(); return;
        }

        if (min_a.same_as(min_b)) {
            min = min_a;
        } else {
            min = Min::make(min_a, min_b);
        }

        if (max_a.same_as(max_b)) {
            max = max_a;
        } else {
            max = Max::make(max_a, max_b);
        }
    }

    void visit(const Load *op) {
        op->index.accept(this);
        if (min.defined() && min.same_as(max)) {
            // If the index is const we can return the load of that index
            min = max = Load::make(op->type, op->name, min, op->image, op->param);
        } else {
            // Otherwise use the bounds of the type
            bounds_of_type(op->type);
        }
    }

    void visit(const Ramp *op) {
        assert(false && "Bounds of vector");
    }

    void visit(const Broadcast *) {
        assert(false && "Bounds of vector");
    }

    void visit(const Call *op) {
        // If the args are const we can return the call of those args
        // for pure functions (extern and image). For other types of
        // functions, the same call in two different places might
        // produce different results (e.g. during the update step of a
        // reduction), so we can't move around call nodes.
        std::vector<Expr> new_args(op->args.size());
        bool const_args = true;
        for (size_t i = 0; i < op->args.size() && const_args; i++) {
            op->args[i].accept(this);
            if (min.defined() && min.same_as(max)) {
                new_args[i] = min;
            } else {
                const_args = false;
            }
        }

        if (const_args && (op->call_type == Call::Image || op->call_type == Call::Extern)) {
            min = max = Call::make(op->type, op->name, new_args, op->call_type,
                                   op->func, op->value_index, op->image, op->param);
        } else if (op->call_type == Call::Intrinsic && op->name == Call::abs) {
            Expr min_a = min, max_a = max;
            min = make_zero(op->type);
            if (min_a.defined() && max_a.defined()) {
                if (op->type.is_uint()) {
                    max = Max::make(cast(op->type, 0-min_a), cast(op->type, max_a));
                } else {
                    max = Max::make(0-min_a, max_a);
                }
            } else {
                // If the argument is unbounded on one side, then the max is unbounded.
                max = Expr();
            }
        } else {
            // Just use the bounds of the type
            bounds_of_type(op->type);
        }
    }

    void visit(const Let *op) {
        op->value.accept(this);
        inner_scope.push(op->name, Interval(min, max));
        op->body.accept(this);
        inner_scope.pop(op->name);
    }

    void visit(const LetStmt *) {
        assert(false && "Bounds of statement");
    }

    void visit(const AssertStmt *) {
        assert(false && "Bounds of statement");
    }

    void visit(const Pipeline *) {
        assert(false && "Bounds of statement");
    }

    void visit(const For *) {
        assert(false && "Bounds of statement");
    }

    void visit(const Store *) {
        assert(false && "Bounds of statement");
    }

    void visit(const Provide *) {
        assert(false && "Bounds of statement");
    }

    void visit(const Allocate *) {
        assert(false && "Bounds of statement");
    }

    void visit(const Realize *) {
        assert(false && "Bounds of statement");
    }

    void visit(const Block *) {
        assert(false && "Bounds of statement");
    }
};

Interval bounds_of_expr_in_scope(Expr expr, const Scope<Interval> &scope) {
    //debug(3) << "computing bounds_of_expr_in_scope " << expr << "\n";
    Bounds b(scope);
    expr.accept(&b);
    //debug(3) << "bounds_of_expr_in_scope " << expr << " = " << simplify(b.min) << ", " << simplify(b.max) << "\n";
    return Interval(b.min, b.max);
}

Interval interval_union(const Interval &a, const Interval &b) {
    Expr max, min;
    debug(3) << "Interval union of " << a.min << ", " << a.max << ",  " << b.min << ", " << b.max << "\n";
    if (a.max.defined() && b.max.defined()) max = Max::make(a.max, b.max);
    if (a.min.defined() && b.min.defined()) min = Min::make(a.min, b.min);
    return Interval(min, max);
}

Region region_union(const Region &a, const Region &b) {
    assert(a.size() == b.size() && "Mismatched dimensionality in region union");
    Region result;
    for (size_t i = 0; i < a.size(); i++) {
        Expr min = Min::make(a[i].min, b[i].min);
        Expr max_a = a[i].min + a[i].extent;
        Expr max_b = b[i].min + b[i].extent;
        Expr max_plus_one = Max::make(max_a, max_b);
        Expr extent = max_plus_one - min;
        result.push_back(Range(simplify(min), simplify(extent)));
        //result.push_back(Range(min, extent));
    }
    return result;
}



void merge_boxes(Box &a, const Box &b) {
    if (b.empty()) {
        return;
    }

    if (a.empty()) {
        a = b;
        return;
    }

    assert(a.size() == b.size());

    for (size_t i = 0; i < a.size(); i++) {
        if (!a[i].min.same_as(b[i].min)) {
            a[i].min = min(a[i].min, b[i].min);
        }
        if (!a[i].max.same_as(b[i].max)) {
            a[i].max = max(a[i].max, b[i].max);
        }
    }
}

// Compute the box produced by a statement
class BoxesTouched : public IRGraphVisitor {

public:
    BoxesTouched(bool calls, bool provides, string fn, const Scope<Interval> &s) :
        func(fn), consider_calls(calls), consider_provides(provides), scope(s) {}

    map<string, Box> boxes;

private:

    string func;
    bool consider_calls, consider_provides;
    Scope<Interval> scope;

    using IRGraphVisitor::visit;

    void visit(const Let *op) {
        if (!consider_calls) return;

        op->value.accept(this);
        Interval value_bounds = bounds_of_expr_in_scope(op->value, scope);
        scope.push(op->name, value_bounds);
        op->body.accept(this);
        scope.pop(op->name);
    }

    void visit(const Call *op) {
        if (!consider_calls) return;

        // Calls inside of an address_of aren't touched, because no
        // actual memory access takes place.
        if (op->call_type == Call::Intrinsic && op->name == Call::address_of) {
            // Visit the args of the inner call
            assert(op->args.size() == 1);
            const Call *c = op->args[0].as<Call>();
            assert(c);
            for (size_t i = 0; i < c->args.size(); i++) {
                c->args[i].accept(this);
            }
            return;
        }

        IRVisitor::visit(op);

        if (op->call_type == Call::Intrinsic ||
            op->call_type == Call::Extern) {
            return;
        }

        string name = op->name;

        Box b(op->args.size());
        for (size_t i = 0; i < op->args.size(); i++) {
            op->args[i].accept(this);
            b[i] = bounds_of_expr_in_scope(op->args[i], scope);
        }
        merge_boxes(boxes[op->name], b);
    }

    void visit(const LetStmt *op) {
        if (consider_calls) {
            op->value.accept(this);
        }
        Interval value_bounds = bounds_of_expr_in_scope(op->value, scope);
        scope.push(op->name, value_bounds);
        op->body.accept(this);
        scope.pop(op->name);
    }

    void visit(const For *op) {
        if (consider_calls) {
            op->min.accept(this);
            op->extent.accept(this);
        }

        Expr min_val, max_val;
        if (scope.contains(op->name + ".loop_min")) {
            min_val = scope.get(op->name + ".loop_min").min;
        } else {
            min_val = bounds_of_expr_in_scope(op->min, scope).min;
        }

        if (scope.contains(op->name + ".loop_max")) {
            max_val = scope.get(op->name + ".loop_max").max;
        } else {
            max_val = bounds_of_expr_in_scope(op->extent, scope).max;
            max_val += bounds_of_expr_in_scope(op->min, scope).max;
            max_val -= 1;
        }

        scope.push(op->name, Interval(min_val, max_val));
        op->body.accept(this);
        scope.pop(op->name);
    }

    void visit(const Provide *op) {
        if (consider_provides) {
            if (op->name == func || func.empty()) {
                Box b(op->args.size());
                for (size_t i = 0; i < op->args.size(); i++) {
                    b[i] = bounds_of_expr_in_scope(op->args[i], scope);
                }
                merge_boxes(boxes[op->name], b);
            }
        }

        if (consider_calls) {
            for (size_t i = 0; i < op->args.size(); i++) {
                op->args[i].accept(this);
            }
            for (size_t i = 0; i < op->values.size(); i++) {
                op->values[i].accept(this);
            }
        }
    }
};

map<string, Box> boxes_touched(Expr e, Stmt s, bool consider_calls, bool consider_provides,
                               string fn, const Scope<Interval> &scope) {
    BoxesTouched b(consider_calls, consider_provides, fn, scope);
    if (e.defined()) {
        e.accept(&b);
    }
    if (s.defined()) {
        s.accept(&b);
    }
    return b.boxes;
}

Box box_touched(Expr e, Stmt s, bool consider_calls, bool consider_provides,
                string fn, const Scope<Interval> &scope) {
    return boxes_touched(e, s, consider_calls, consider_provides, fn, scope)[fn];
}

map<string, Box> boxes_required(Expr e, const Scope<Interval> &scope) {
    return boxes_touched(e, Stmt(), true, false, "", scope);
}

Box box_required(Expr e, string fn, const Scope<Interval> &scope) {
    return box_touched(e, Stmt(), true, false, fn, scope);
}

map<string, Box> boxes_required(Stmt s, const Scope<Interval> &scope) {
    return boxes_touched(Expr(), s, true, false, "", scope);
}

Box box_required(Stmt s, string fn, const Scope<Interval> &scope) {
    return box_touched(Expr(), s, true, false, fn, scope);
}

map<string, Box> boxes_required(Expr e) {
    const Scope<Interval> scope;
    return boxes_touched(e, Stmt(), true, false, "", scope);
}

Box box_required(Expr e, string fn) {
    const Scope<Interval> scope;
    return box_touched(e, Stmt(), true, false, fn, scope);
}

map<string, Box> boxes_required(Stmt s) {
    const Scope<Interval> scope;
    return boxes_touched(Expr(), s, true, false, "", scope);
}

Box box_required(Stmt s, string fn) {
    const Scope<Interval> scope;
    return box_touched(Expr(), s, true, false, fn, scope);
}


map<string, Box> boxes_provided(Expr e, const Scope<Interval> &scope) {
    return boxes_touched(e, Stmt(), false, true, "", scope);
}

Box box_provided(Expr e, string fn, const Scope<Interval> &scope) {
    return box_touched(e, Stmt(), false, true, fn, scope);
}

map<string, Box> boxes_provided(Stmt s, const Scope<Interval> &scope) {
    return boxes_touched(Expr(), s, false, true, "", scope);
}

Box box_provided(Stmt s, string fn, const Scope<Interval> &scope) {
    return box_touched(Expr(), s, false, true, fn, scope);
}

map<string, Box> boxes_provided(Expr e) {
    const Scope<Interval> scope;
    return boxes_touched(e, Stmt(), false, true, "", scope);
}

Box box_provided(Expr e, string fn) {
    const Scope<Interval> scope;
    return box_touched(e, Stmt(), false, true, fn, scope);
}

map<string, Box> boxes_provided(Stmt s) {
    const Scope<Interval> scope;
    return boxes_touched(Expr(), s, false, true, "", scope);
}

Box box_provided(Stmt s, string fn) {
    const Scope<Interval> scope;
    return box_touched(Expr(), s, false, true, fn, scope);
}


map<string, Box> boxes_touched(Expr e, const Scope<Interval> &scope) {
    return boxes_touched(e, Stmt(), true, true, "", scope);
}

Box box_touched(Expr e, string fn, const Scope<Interval> &scope) {
    return box_touched(e, Stmt(), true, true, fn, scope);
}

map<string, Box> boxes_touched(Stmt s, const Scope<Interval> &scope) {
    return boxes_touched(Expr(), s, true, true, "", scope);
}

Box box_touched(Stmt s, string fn, const Scope<Interval> &scope) {
    return box_touched(Expr(), s, true, true, fn, scope);
}

map<string, Box> boxes_touched(Expr e) {
    const Scope<Interval> scope;
    return boxes_touched(e, Stmt(), true, true, "", scope);
}

Box box_touched(Expr e, string fn) {
    const Scope<Interval> scope;
    return box_touched(e, Stmt(), true, true, fn, scope);
}

map<string, Box> boxes_touched(Stmt s) {
    const Scope<Interval> scope;
    return boxes_touched(Expr(), s, true, true, "", scope);
}

Box box_touched(Stmt s, string fn) {
    const Scope<Interval> scope;
    return box_touched(Expr(), s, true, true, fn, scope);
}

void check(const Scope<Interval> &scope, Expr e, Expr correct_min, Expr correct_max) {
    Interval result = bounds_of_expr_in_scope(e, scope);
    if (result.min.defined()) result.min = simplify(result.min);
    if (result.max.defined()) result.max = simplify(result.max);
    bool success = true;
    if (!equal(result.min, correct_min)) {
        std::cout << "In bounds of " << e << ":\n"
                  << "Incorrect min: " << result.min << '\n'
                  << "Should have been: " << correct_min << '\n';
        success = false;
    }
    if (!equal(result.max, correct_max)) {
        std::cout << "In bounds of " << e << ":\n"
                  << "Incorrect max: " << result.max << '\n'
                  << "Should have been: " << correct_max << '\n';
        success = false;
    }
    if (!success) {
        std::cout << "Bounds test failed\n";
        assert(false);
    }
}

void bounds_test() {
    Scope<Interval> scope;
    Var x("x"), y("y");
    scope.push("x", Interval(Expr(0), Expr(10)));

    check(scope, x, 0, 10);
    check(scope, x+1, 1, 11);
    check(scope, (x+1)*2, 2, 22);
    check(scope, x*x, 0, 100);
    check(scope, 5-x, -5, 5);
    check(scope, x*(5-x), -50, 50); // We don't expect bounds analysis to understand correlated terms
    check(scope, Select::make(x < 4, x, x+100), 0, 110);
    check(scope, x+y, y, y+10);
    check(scope, x*y, select(y < 0, y*10, 0), select(y < 0, 0, y*10));
    check(scope, x/(x+y), Expr(), Expr());
    check(scope, 11/(x+1), 1, 11);
    check(scope, Load::make(Int(8), "buf", x, Buffer(), Parameter()), cast(Int(8), -128), cast(Int(8), 127));
    check(scope, y + (Let::make("y", x+3, y - x + 10)), y + 3, y + 23); // Once again, we don't know that y is correlated with x
    check(scope, clamp(1/(x-2), x-10, x+10), -10, 20);

    // Check some operations that may overflow
    check(scope, (cast<uint8_t>(x)+250), cast<uint8_t>(0), cast<uint8_t>(255));
    check(scope, (cast<uint8_t>(x)+10)*20, cast<uint8_t>(0), cast<uint8_t>(255));
    check(scope, (cast<uint8_t>(x)+10)*(cast<uint8_t>(x)+5), cast<uint8_t>(0), cast<uint8_t>(255));
    check(scope, (cast<uint8_t>(x)+10)-(cast<uint8_t>(x)+5), cast<uint8_t>(0), cast<uint8_t>(255));

    // Check some operations that we should be able to prove do not overflow
    check(scope, (cast<uint8_t>(x)+240), cast<uint8_t>(240), cast<uint8_t>(250));
    check(scope, (cast<uint8_t>(x)+10)*10, cast<uint8_t>(100), cast<uint8_t>(200));
    check(scope, (cast<uint8_t>(x)+10)*(cast<uint8_t>(x)), cast<uint8_t>(0), cast<uint8_t>(200));
    check(scope, (cast<uint8_t>(x)+20)-(cast<uint8_t>(x)+5), cast<uint8_t>(5), cast<uint8_t>(25));

    vector<Expr> input_site_1 = vec(2*x);
    vector<Expr> input_site_2 = vec(2*x+1);
    vector<Expr> output_site = vec(x+1);

    Buffer in(Int(32), vec(10), NULL, "input");

    Stmt loop = For::make("x", 3, 10, For::Serial,
                          Provide::make("output",
                                        vec(Add::make(
                                                Call::make(in, input_site_1),
                                                Call::make(in, input_site_2))),
                                        output_site));

    map<string, Box> r;
    r = boxes_required(loop);
    assert(r.find("output") == r.end());
    assert(r.find("input") != r.end());
    assert(equal(simplify(r["input"][0].min), 6));
    assert(equal(simplify(r["input"][0].max), 25));
    r = boxes_provided(loop);
    assert(r.find("output") != r.end());
    assert(equal(simplify(r["output"][0].min), 4));
    assert(equal(simplify(r["output"][0].max), 13));

    Box r2 = vec(Interval(Expr(5), Expr(19)));
    merge_boxes(r2, r["output"]);
    assert(equal(simplify(r2[0].min), 4));
    assert(equal(simplify(r2[0].max), 19));

    std::cout << "Bounds test passed" << std::endl;
}

}
}

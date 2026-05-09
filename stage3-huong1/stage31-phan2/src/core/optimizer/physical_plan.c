/* physical_plan.c — Physical plan utilities */
#include "physical_plan.h"
#include <stdio.h>
#include <string.h>

const char* pop_type_name(PhysicalOpType t) {
    switch (t) {
    case POP_SEQ_SCAN:          return "SeqScan";
    case POP_INDEX_SCAN:        return "IndexScan";
    case POP_FILTER:            return "Filter";
    case POP_PROJECT:           return "Project";
    case POP_NESTED_LOOP_JOIN:  return "NestedLoopJoin";
    case POP_HASH_JOIN:         return "HashJoin";
    case POP_SORT_MERGE_JOIN:   return "SortMergeJoin";
    case POP_HASH_AGGREGATE:    return "HashAggregate";
    case POP_STREAM_AGGREGATE:  return "StreamAggregate";
    case POP_SORT:              return "Sort";
    case POP_LIMIT:             return "Limit";
    default:                    return "Unknown";
    }
}

static void print_indent_phys(int depth) {
    for (int i = 0; i < depth; i++) printf("    ");
    if (depth > 0) printf("└─ ");
}

void physical_plan_print(const PhysicalPlan *plan, int depth) {
    if (!plan) return;
    if (depth == 0) printf("PhysicalPlan:\n");
    print_indent_phys(depth);

    printf("%s", pop_type_name(plan->type));

    switch (plan->type) {
    case POP_SEQ_SCAN:
        printf("(%s)", plan->seq_scan.collection_name);
        break;
    case POP_INDEX_SCAN:
        printf("(%s.%s)", plan->index_scan.collection_name, plan->index_scan.index_col);
        break;
    case POP_FILTER: {
        const Condition *c = plan->filter.predicate;
        if (c && c->type == COND_CMP) {
            const char *op = "?";
            switch (c->op) {
            case TOK_OP_BG: op = "=";  break;
            case TOK_OP_KC: op = "!="; break;
            case TOK_OP_LH: op = "<";  break;
            case TOK_OP_BH: op = ">";  break;
            case TOK_OP_LHB: op = "<="; break;
            case TOK_OP_BHB: op = ">="; break;
            case TOK_OP_XAU: op = "contains"; break;
            default: break;
            }
            if (c->value.type == VAL_NUM)
                printf("(%s %s %g)", c->field, op, c->value.num);
            else
                printf("(%s %s '%s')", c->field, op, c->value.str);
        } else {
            printf("(...)");
        }
        break;
    }
    case POP_NESTED_LOOP_JOIN:
    case POP_HASH_JOIN:
    case POP_SORT_MERGE_JOIN:
        printf("(%s = %s)", plan->join.left_col, plan->join.right_col);
        break;
    case POP_SORT: {
        printf("(");
        int first = 1;
        for (SortField *sf = plan->sort.fields; sf; sf = sf->next) {
            if (!first) printf(", ");
            printf("%s %s", sf->field, sf->descending ? "DESC" : "ASC");
            first = 0;
        }
        printf(")");
        break;
    }
    case POP_LIMIT:
        if (plan->limit.skip > 0)
            printf("(%d skip=%d)", plan->limit.limit, plan->limit.skip);
        else
            printf("(%d)", plan->limit.limit);
        break;
    case POP_HASH_AGGREGATE:
    case POP_STREAM_AGGREGATE:
        printf("(group_by %s)", plan->aggregate.group_by_field);
        break;
    default:
        break;
    }

    printf("  [cost=%.2f  rows=%.0f]\n",
           plan->estimated_cost, plan->estimated_rows);

    physical_plan_print(plan->left,  depth + 1);
    physical_plan_print(plan->right, depth + 1);
}

double physical_plan_total_cost(const PhysicalPlan *plan) {
    if (!plan) return 0;
    return plan->estimated_cost;
}

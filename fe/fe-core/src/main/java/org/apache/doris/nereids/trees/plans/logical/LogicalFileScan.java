// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.trees.plans.logical;

import org.apache.doris.catalog.external.ExternalTable;
import org.apache.doris.nereids.memo.GroupExpression;
import org.apache.doris.nereids.properties.LogicalProperties;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.plans.ObjectId;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.PlanType;
import org.apache.doris.nereids.trees.plans.visitor.PlanVisitor;
import org.apache.doris.nereids.util.Utils;

import com.google.common.base.Preconditions;
import com.google.common.collect.Sets;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;

/**
 * Logical file scan for external catalog.
 */
public class LogicalFileScan extends LogicalRelation {

    private final Set<Expression> conjuncts;

    /**
     * Constructor for LogicalFileScan.
     */
    public LogicalFileScan(ObjectId id, ExternalTable table, List<String> qualifier,
                           Optional<GroupExpression> groupExpression,
                           Optional<LogicalProperties> logicalProperties,
                           Set<Expression> conjuncts) {
        super(id, PlanType.LOGICAL_FILE_SCAN, table, qualifier,
                groupExpression, logicalProperties);
        this.conjuncts = conjuncts;
    }

    public LogicalFileScan(ObjectId id, ExternalTable table, List<String> qualifier) {
        this(id, table, qualifier, Optional.empty(), Optional.empty(), Sets.newHashSet());
    }

    @Override
    public ExternalTable getTable() {
        Preconditions.checkArgument(table instanceof ExternalTable);
        return (ExternalTable) table;
    }

    @Override
    public String toString() {
        return Utils.toSqlString("LogicalFileScan",
            "qualified", qualifiedName(),
            "output", getOutput()
        );
    }

    @Override
    public boolean equals(Object o) {
        return super.equals(o) && Objects.equals(conjuncts, ((LogicalFileScan) o).conjuncts);
    }

    @Override
    public LogicalFileScan withGroupExpression(Optional<GroupExpression> groupExpression) {
        return new LogicalFileScan(id, (ExternalTable) table, qualifier, groupExpression,
                Optional.of(getLogicalProperties()), conjuncts);
    }

    @Override
    public Plan withGroupExprLogicalPropChildren(Optional<GroupExpression> groupExpression,
            Optional<LogicalProperties> logicalProperties, List<Plan> children) {
        return new LogicalFileScan(id, (ExternalTable) table, qualifier, groupExpression, logicalProperties, conjuncts);
    }

    public LogicalFileScan withConjuncts(Set<Expression> conjuncts) {
        return new LogicalFileScan(id, (ExternalTable) table, qualifier, groupExpression,
            Optional.of(getLogicalProperties()), conjuncts);
    }

    @Override
    public <R, C> R accept(PlanVisitor<R, C> visitor, C context) {
        return visitor.visitLogicalFileScan(this, context);
    }

    public Set<Expression> getConjuncts() {
        return this.conjuncts;
    }
}

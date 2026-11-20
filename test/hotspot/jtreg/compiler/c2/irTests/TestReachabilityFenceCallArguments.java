/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package compiler.c2.irTests;

import java.lang.ref.Reference;

import compiler.lib.ir_framework.*;

/*
 * @test
 * @bug 8290892
 * @summary Call arguments must not suppress reachability edges, but debug roots should.
 * @requires vm.compiler2.enabled
 * @library /test/lib /
 * @run driver compiler.c2.irTests.TestReachabilityFenceCallArguments
 */
public class TestReachabilityFenceCallArguments {
    private static int counter;

    @DontInline
    static void consume(Object value) {
        counter++;
    }

    @Test
    @IR(counts = {IRNode.STATIC_CALL_OF_METHOD, "consume", "1",
                  IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.FINAL_CODE)
    static void testArgumentOnly(Object value) {
        // StressReachabilityFences adds a fence at method exit without making value
        // live in the bytecode. At this call, value is an argument but not a debug root.
        // A reachability edge must survive expansion and become a fence after the call.
        consume(value);
    }

    @Test
    @IR(counts = {IRNode.STATIC_CALL_OF_METHOD, "consume", "1",
                  IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.FINAL_CODE)
    static void testArgumentAndDebugRoot(Object value) {
        consume(value);
        // The explicit bytecode use keeps value in the call's debug information.
        // This existing root makes an additional reachability edge unnecessary.
        Reference.reachabilityFence(value);
    }

    @Run(test = {"testArgumentOnly", "testArgumentAndDebugRoot"})
    public void runTests() {
        Object value = new Object();
        testArgumentOnly(value);
        testArgumentAndDebugRoot(value);
    }

    public static void main(String[] args) {
        TestFramework.runWithFlags("-XX:+UnlockDiagnosticVMOptions",
                                   "-XX:+StressReachabilityFences",
                                   "-XX:+OptimizeReachabilityFences");
    }
}

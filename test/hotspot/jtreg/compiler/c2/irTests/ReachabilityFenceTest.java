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

import java.lang.ref.Cleaner;
import java.lang.ref.Reference;

import compiler.lib.ir_framework.*;
import jdk.internal.misc.Unsafe;

/*
 * @test
 * @bug 8290892
 * @summary IR tests on reachability fence-related loop transformations.
 * @modules java.base/jdk.internal.misc
 * @library /test/lib /
 * @run driver compiler.c2.irTests.ReachabilityFenceTest
 */
public class ReachabilityFenceTest {
    private static int SIZE = 100;

    static class WrapperObj {
        final Object obj;

        WrapperObj(Object obj) {
            this.obj = obj;
        }
    }

    static class WrapperTuple {
        final Object first;
        final Object second;
        final Object third;

        WrapperTuple(Object first, Object second, Object third) {
            this.first = first;
            this.second = second;
            this.third = third;
        }
    }

    static class WrapperPair {
        final Object obj;
        final int i;

        WrapperPair(Object obj, int i) {
            this.obj = obj;
            this.i = i;
        }
    }

    /* ===================================== On-heap version ===================================== */

    private static int[] a = new int[SIZE];
    private static int[] b = new int[SIZE];
    private static int[] c = new int[SIZE];

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopInt() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                Reference.reachabilityFence(a);
                Reference.reachabilityFence(b);
                Reference.reachabilityFence(c);
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrapped() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // After scalarization, each RF is migrated to corresponding object field.
                Reference.reachabilityFence(new WrapperObj(a)); // 1 -> 1 RF
                Reference.reachabilityFence(new WrapperObj(b)); // 1 -> 1 RF
                Reference.reachabilityFence(new WrapperObj(c)); // 1 -> 1 RF
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedTuple() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // After scalarization, each object field gets its own RF.
                Reference.reachabilityFence(new WrapperTuple(a, b, c)); // 1 -> 3 RFs
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedTupleNull() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // Null components are redundant.
                Reference.reachabilityFence(new WrapperTuple(a, null, null)); // 1 -> 3 -> 1 RF
                Reference.reachabilityFence(new WrapperTuple(null, b, null)); // 1 -> 3 -> 1 RF
                Reference.reachabilityFence(new WrapperTuple(null, null, c)); // 1 -> 3 -> 1 RF
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedTupleLocal() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // RFs on freshly allocated empty objects are redundant.
                Reference.reachabilityFence(new WrapperTuple(           a, new Object(), new Object())); // 1 -> 3 -> 1 RF
                Reference.reachabilityFence(new WrapperTuple(new Object(),            b, new Object())); // 1 -> 3 -> 1 RF
                Reference.reachabilityFence(new WrapperTuple(new Object(), new Object(),            c)); // 1 -> 3 -> 1 RF
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedPairLI() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // Only object fields matter.
                Reference.reachabilityFence(new WrapperPair(a, i)); // 1 -> 1 RF
                Reference.reachabilityFence(new WrapperPair(b, i)); // 1 -> 1 RF
                Reference.reachabilityFence(new WrapperPair(c, i)); // 1 -> 1 RF
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedArray() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // After scalarization, each array element has its own RF.
                Reference.reachabilityFence(new Object[] { a, b, c }); // 1 -> 3 RFs
            }
        }
    }

    /* ===================================== Off-heap version ===================================== */

    static class OffHeapBuffer  {
        private static Unsafe UNSAFE = Unsafe.getUnsafe();

        private final long payload;

        public final long size;

        OffHeapBuffer() {
            size = SIZE;
            payload = UNSAFE.allocateMemory(SIZE);

            Cleaner.create().register(this, () -> {
                UNSAFE.freeMemory(payload);
            });
        }

        public void put(long offset, byte b) {
            try {
                UNSAFE.putByte(payload + offset, b);
            } finally {
                Reference.reachabilityFence(this);
            }
        }

        public byte get(long offset) {
            try {
                return UNSAFE.getByte(payload + offset);
            } finally {
                Reference.reachabilityFence(this);
            }
        }
    }

    private static OffHeapBuffer bufA = new OffHeapBuffer();
    private static OffHeapBuffer bufB = new OffHeapBuffer();
    private static OffHeapBuffer bufC = new OffHeapBuffer();

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopLong() {
        for (long i = 0; i < bufA.size; i++) {
            byte a = bufA.get(i);     // +1 RF
            byte b = bufB.get(i);     // +1 RF
            bufC.put(i, (byte)(a+b)); // +1 RF
        }
    }

    /* ============================================================================================ */

    public static void main(String[] args) throws Throwable {
        TestFramework framework = new TestFramework();
        framework.addFlags("--add-exports", "java.base/jdk.internal.misc=ALL-UNNAMED");
        framework.start();
    }
}

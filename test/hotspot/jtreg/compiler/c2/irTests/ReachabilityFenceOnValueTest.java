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
import jdk.internal.value.ValueClass;

/*
 * @test
 * @bug 8290892
 * @summary IR tests on reachability fence-related loop transformations.
 * @enablePreview
 * @modules java.base/jdk.internal.misc
 *          java.base/jdk.internal.value
 * @library /test/lib /
 * @run driver compiler.c2.irTests.ReachabilityFenceOnValueTest
 */
public class ReachabilityFenceOnValueTest {
    private static int SIZE = 100;

    static value class WrapperObj {
        final Object obj;

        WrapperObj(Object obj) {
            this.obj = obj;
        }
    }

    static value class WrapperTuple {
        final Object first;
        final Object second;
        final Object third;

        WrapperTuple(Object first, Object second, Object third) {
            this.first = first;
            this.second = second;
            this.third = third;
        }
    }

    static value class WrapperPair {
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
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.BEFORE_LOOP_OPTS)
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
    @IR(counts = {IRNode.REACHABILITY_FENCE, "9"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
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
    @IR(counts = {IRNode.REACHABILITY_FENCE,   "3"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, ">=3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE,   "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE,   "3"}, phase = CompilePhase.FINAL_CODE)
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
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedArray0() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // FIXME: flat arrays
                // After scalarization, each array element has its own RF.
                Reference.reachabilityFence(new Object[] { new WrapperObj(a), new WrapperObj(b), new WrapperObj(c) }); // 1 -> 3 RFs
            }
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedArray1() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // FIXME: flat arrays
                // After scalarization, each array element has its own RF.
                Reference.reachabilityFence(new WrapperObj[] { new WrapperObj(a), new WrapperObj(b), new WrapperObj(c) }); // 1 -> 3 RFs
            }
        }
    }

    @ForceInline
    static Object wrapAsReferenceArray(Object... referents) {
        WrapperObj[] wrapperArray = (WrapperObj[]) ValueClass.newReferenceArray(WrapperObj.class, referents.length);
        for (int i = 0; i < referents.length; i++) {
            wrapperArray[i] = new WrapperObj(referents[i]);
        }
        return wrapperArray;
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "4"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "4"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "4"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "4"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedArray5() {
        Object topRefArray = wrapAsReferenceArray(a, b, c);
        try {
            for (int i = 0; i < a.length; i++) {
                Object refArray = wrapAsReferenceArray(a, b, c);
                try {
                    c[i] = a[i] + b[i];
                } finally {
                    Reference.reachabilityFence(wrapAsReferenceArray(a, b, c)); // 1 -> 0 RFs
                    Reference.reachabilityFence(refArray); // 1 -> 0 RFs
                }
            }
        } finally {
            Reference.reachabilityFence(wrapAsReferenceArray(a, b, c)); // 1 -> 0 RFs
            Reference.reachabilityFence(topRefArray); // 1 -> 0 RFs
        }
    }

    @Test
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.AFTER_PARSING)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "1"}, phase = CompilePhase.PHASEIDEAL_BEFORE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_ITERATIVE_EA)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.AFTER_LOOP_OPTS)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "0"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.FINAL_CODE)
    static void testCountedLoopIntWrappedArray2() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // FIXME: flat arrays
                // After scalarization, each array element has its own RF.
                WrapperObj[] flatArray = (WrapperObj[]) ValueClass.newNullRestrictedAtomicArray(WrapperObj.class, 3, new WrapperObj(null));
                flatArray[0] = new WrapperObj(a);
                flatArray[1] = new WrapperObj(b);
                flatArray[2] = new WrapperObj(c);

                Reference.reachabilityFence(flatArray); // 1 -> 3 RFs
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
    static void testCountedLoopIntWrappedArray3() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // FIXME: flat arrays
                // After scalarization, each array element has its own RF.
                WrapperObj[] flatArray = (WrapperObj[]) ValueClass.newNullRestrictedNonAtomicArray(WrapperObj.class, 3, new WrapperObj(null));
                flatArray[0] = new WrapperObj(a);
                flatArray[1] = new WrapperObj(b);
                flatArray[2] = new WrapperObj(c);

                Reference.reachabilityFence(flatArray); // 1 -> 3 RFs
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
    static void testCountedLoopIntWrappedArray4() {
        for (int i = 0; i < a.length; i++) {
            try {
                c[i] = a[i] + b[i];
            } finally {
                // FIXME: flat arrays
                // After scalarization, each array element has its own RF.
                WrapperObj[] flatArray = (WrapperObj[]) ValueClass.newNullableAtomicArray(WrapperObj.class, 3);
                flatArray[0] = new WrapperObj(a);
                flatArray[1] = new WrapperObj(b);
                flatArray[2] = new WrapperObj(c);

                Reference.reachabilityFence(flatArray); // 1 -> 3 RFs
            }
        }
    }

    /* ===================================== Off-heap version ===================================== */

    static value class OffHeapBuffer  {
        private static Unsafe UNSAFE = Unsafe.getUnsafe();

        private final Object cookie = new Object();

        private final long payload;

        public final long size;

        OffHeapBuffer() {
            size = SIZE;
            payload = UNSAFE.allocateMemory(SIZE);

            final long p = payload;
            Cleaner.create().register(cookie, () -> {
                UNSAFE.freeMemory(p);
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
    @IR(counts = {IRNode.REACHABILITY_FENCE, "3"}, phase = CompilePhase.EXPAND_REACHABILITY_FENCES)
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
        framework.addFlags("--enable-preview",
                           "--add-exports", "java.base/jdk.internal.misc=ALL-UNNAMED",
                           "--add-exports", "java.base/jdk.internal.value=ALL-UNNAMED");
        framework.start();
    }
}

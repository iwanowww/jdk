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

/*
 * @test
 * @bug 8290892 8388441
 * @summary Exercise late substitutability expansion on a call with a reachability edge
 * @requires vm.compiler2.enabled
 * @enablePreview
 * @run main/othervm -XX:-TieredCompilation -Xbatch -XX:+UnlockDiagnosticVMOptions
 *                   -XX:+StressLateSubstitutabilityExpansion
 *                   -XX:CompileCommand=compileonly,${test.main.class}::test
 *                   ${test.main.class}
 */

import java.lang.ref.Reference;

public class TestLateSubstitutabilityWithReachabilityFence {
    record Box(Object value) { }
    value record MyValue(Object value) { }

    static Object first = new Object();
    static Object second = new Object();

    static boolean same(Object left, Object right) {
        return left == right;
    }

    // The merged referent is used only by the fence. RF expansion adds the branch
    // referents as non-debug edges to the still-live substitutability call.
    static boolean test(boolean branch, boolean choice) {
        Object referent;
        boolean result;
        if (branch) {
            referent = first;
            Box box = choice ? new Box(new MyValue(null)) : new Box(null);
            if (box.value == null) {
                box = new Box(new MyValue(null));
            }
            result = same((MyValue) box.value, box.value);
        } else {
            referent = second;
            result = true;
        }
        Reference.reachabilityFence(referent);
        return result;
    }

    public static void main(String[] args) {
        // Profile acmp with identity operands before compiling test().
        for (int i = 0; i < 10_000; i++) {
            same(args, args);
        }
        for (int i = 0; i < 50_000; i++) {
            if (!test((i & 1) == 0, (i & 2) == 0)) {
                throw new AssertionError(i);
            }
        }
    }
}

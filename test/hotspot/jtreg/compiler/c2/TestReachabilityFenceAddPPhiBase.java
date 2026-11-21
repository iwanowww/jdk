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
 * or visit www.oracle.com if you have any questions.
 *
 */

import java.lang.ref.Reference;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import jdk.internal.misc.Unsafe;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

/*
 * @test
 * @summary A reachability-fenced oop load can use an AddP with distinct Phi base and address inputs
 * @requires vm.debug == true & vm.compiler2.enabled
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @run driver TestReachabilityFenceAddPPhiBase
 */
public class TestReachabilityFenceAddPPhiBase {
    private static final Unsafe U = Unsafe.getUnsafe();
    private static final long BASE_OFFSET = U.arrayBaseOffset(Object[].class);
    private static final long INDEX_SCALE = U.arrayIndexScale(Object[].class);
    private static volatile Object sink1;
    private static volatile Object sink2;

    private static Object test(Object[] a, Object[] b, long delta,
                               boolean chooseA, boolean useFirst) {
        if (a == null || b == null) {
            throw new AssertionError();
        }
        Object[] selected;
        long offset;
        if (chooseA) {
            selected = a;
            offset = 0;
        } else {
            selected = b;
            offset = delta;
        }
        long address = BASE_OFFSET + offset;
        // Two uses on different control paths cause C2 to split the address
        // computation. One path retains the base; the other adds delta.
        if (useFirst) {
            sink1 = U.getReference(selected, address);
        } else {
            sink2 = U.getReference(selected, address);
        }
        Thread.yield();
        Object value = U.getReference(selected, address);
        Reference.reachabilityFence(value);
        return value;
    }

    private static void runChild() {
        Object[] a = {new Object(), new Object()};
        Object[] b = {new Object(), new Object()};
        for (int i = 0; i < 20_000; i++) {
            boolean chooseA = (i & 1) == 0;
            long delta = (i & 2) == 0 ? 0 : INDEX_SCALE;
            Object expected = chooseA ? a[0] : b[(int) (delta / INDEX_SCALE)];
            if (test(a, b, delta, chooseA, (i & 4) == 0) != expected) {
                throw new AssertionError("Wrong value for iteration=" + i);
            }
        }
    }

    private static boolean hasDistinctPhiAddressLoad(Map<Integer, String> names,
                                                     Map<Integer, int[]> addpInputs,
                                                     List<Integer> loadAddresses) {
        for (int address : loadAddresses) {
            int[] inputs = addpInputs.get(address);
            if (inputs != null && inputs[0] != inputs[1] &&
                    "Phi".equals(names.get(inputs[0])) &&
                    "Phi".equals(names.get(inputs[1]))) {
                System.out.println("Found Load -> AddP " + address +
                                   " with Base=Phi " + inputs[0] +
                                   " and Address=Phi " + inputs[1]);
                return true;
            }
        }
        return false;
    }

    private static boolean hasDistinctPhiAddressLoad(String output) {
        Map<Integer, String> names = new HashMap<>();
        Map<Integer, int[]> addpInputs = new HashMap<>();
        List<Integer> loadAddresses = new ArrayList<>();
        boolean inGraph = false;
        for (String line : output.split("\\R")) {
            if (line.startsWith("AFTER: PrintIdeal")) {
                if (hasDistinctPhiAddressLoad(names, addpInputs, loadAddresses)) {
                    return true;
                }
                names.clear();
                addpInputs.clear();
                loadAddresses.clear();
                inGraph = true;
                continue;
            }
            int separator = line.indexOf("  ===");
            int outputs = line.indexOf("[[", separator + 5);
            if (!inGraph || separator < 0 || outputs < 0) {
                continue;
            }
            String[] header = line.substring(0, separator).trim().split("\\s+");
            if (header.length != 2) {
                continue;
            }
            int id = Integer.parseInt(header[0]);
            String name = header[1];
            String[] inputs = line.substring(separator + 5, outputs).trim().split("\\s+");
            names.put(id, name);
            if (name.equals("AddP") && inputs.length > 2) {
                addpInputs.put(id, new int[]{Integer.parseInt(inputs[1]),
                                              Integer.parseInt(inputs[2])});
            } else if ((name.equals("LoadN") || name.equals("LoadP")) &&
                       inputs.length > 2) {
                loadAddresses.add(Integer.parseInt(inputs[2]));
            }
        }
        return hasDistinctPhiAddressLoad(names, addpInputs, loadAddresses);
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            runChild();
            return;
        }
        OutputAnalyzer output = ProcessTools.executeTestJava(
                "-Xbatch", "-XX:-TieredCompilation", "-XX:+UnlockDiagnosticVMOptions",
                "-XX:+PrintIdeal", "-XX:CompileCommand=quiet",
                "-XX:CompileCommand=compileonly,TestReachabilityFenceAddPPhiBase::test",
                "--add-exports=java.base/jdk.internal.misc=ALL-UNNAMED",
                TestReachabilityFenceAddPPhiBase.class.getName(), "child");
        output.shouldHaveExitValue(0);
        if (!hasDistinctPhiAddressLoad(output.getOutput())) {
            throw new AssertionError("No Load -> AddP with distinct Phi base and address found in PrintIdeal output");
        }
    }
}

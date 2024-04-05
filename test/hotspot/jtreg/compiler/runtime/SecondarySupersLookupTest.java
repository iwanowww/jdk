/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
 * @modules java.base/jdk.internal.org.objectweb.asm
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -Xbatch -XX:-TieredCompilation -XX:-UseSecondarySupersCache
 *                   -DWORKERS=1 -DTEST_COUNT=1000
 *                      SecondarySupersLookupTest
 */

import jdk.internal.org.objectweb.asm.ClassWriter;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Random;
import java.util.concurrent.*;

import static jdk.internal.org.objectweb.asm.Opcodes.*;

/*
 * $ java --add-exports java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED \
 *   -Xbatch -XX:-TieredCompilation -XX:+UnlockDiagnosticVMOptions -XX:-VerifySecondarySupers -XX:-UseSecondarySupersCache \
 *      SecondarySupersLookupTest
 */

public class SecondarySupersLookupTest {
    static final int SIZE = 64;
    static final int MASK = SIZE-1;

    static final Class<?>[][] INTERFACES = new Class[SIZE][SIZE];
    static final int[]        POSITIONS  = new   int[SIZE];
    static final Random RAND;
    static final boolean VERBOSE;

    static int computeHash(String name) {
        int h = 0;
        byte[] bytes = name.getBytes();
        for (int i = 0; i < bytes.length; i++) {
            h = 31*h + (bytes[i] & 0xFF);
        }
        return h;
    }

    static int computeHashSlot(String name) {
        int hash_code = computeHash(name);
        int hash_shift = 32 - 6;
        hash_code = (int)((hash_code * 2654435769L) >>> hash_shift);
        return (hash_code & MASK);
    }

    static {
        VERBOSE = Boolean.getBoolean("VERBOSE");

        long seed = Long.getLong("SEED", -1);
        if (seed == -1) {
            seed = (new Random()).nextLong();
        }
        System.out.printf("-DSEED=%d\n", seed);
        RAND = new Random(seed);
    }

    static byte[] generateInterfaceClassFile(String name) {
        ClassWriter classWriter = new ClassWriter(0);

        classWriter.visit(66, ACC_ABSTRACT | ACC_INTERFACE, name, null, "java/lang/Object", null);
        classWriter.visitEnd();

        return classWriter.toByteArray();
    }

    static byte[] generateTestClassFile(String[] supers) {
        ClassWriter classWriter = new ClassWriter(0);

        classWriter.visit(66, ACC_PUBLIC | ACC_SUPER, "Test", null, "java/lang/Object", supers);
        classWriter.visitEnd();

        return classWriter.toByteArray();
    }

    static Class<?> generateInteface(String name) {
        try {
            byte[] classFile = generateInterfaceClassFile(name);
            return MethodHandles.lookup().defineClass(classFile);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    static Class<?> generateTest(String[] supers) {
        try {
            byte[] classFile = generateTestClassFile(supers);
            return MethodHandles.lookup().defineHiddenClass(classFile, false).lookupClass();
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    static Class<?> createRandomTest() {
        int count = RAND.nextInt(SIZE);

        int[]    slots  = new int[count];
        String[] supers = new String[count];
        for (int i = 0; i < count; i++) {
            int slot = RAND.nextInt(SIZE);
            int pos = (POSITIONS[slot]++) & MASK;
            supers[i] = INTERFACES[slot][pos].getName();
            slots[i] = slot;
        }

        if (VERBOSE) {
            System.out.print("= {");
            for (int i = 0; i < count; i++) {
                System.out.printf("%s%2d", (i == 0 ? " " : ", "), slots[i]);
            }
            System.out.print("}");
        }

        return generateTest(supers);
    }

    static boolean test(Class<?> superk, Class<?> subk) {
        return superk.isAssignableFrom(subk);
    }

    static final MethodHandle TEST_MH;
    static {
        try {
            TEST_MH = MethodHandles.lookup().findStatic(SecondarySupersLookupTest.class, "test",
                                                        MethodType.methodType(boolean.class, Class.class, Class.class));
        } catch (NoSuchMethodException | IllegalAccessException e) {
            throw new AssertionError(e);
        }
    }

    static void run(MethodHandle mh, Class<?> subk, Class<?> superk) {
        boolean expected = test(superk, subk);
        mh = mh.bindTo(superk);

        try {
            for (int j = 0; j < 20_000; j++) {
                boolean result = (boolean) mh.invokeExact(subk);
                if (result != expected) {
                    throw new AssertionError("mismatch");
                }
            }
        } catch (Throwable e) {
            throw new AssertionError(e);
        }
    }

    public static void main(String[] args) throws Exception {
        System.out.print("Populating test inputs...");
        for (int count = 0, i = 0; count < 64*64; i++) {
            if (VERBOSE) {
                if ((i % 1000) == 0) {
                    System.out.printf("%d: ", i);
                    for (int pos : POSITIONS) {
                        System.out.printf(" %d", pos);
                    }
                    System.out.println();
                }
            }
            int id = RAND.nextInt();
            String name = String.format("Intf%08X", id);
            int slot = computeHashSlot(name);
            if (POSITIONS[slot] < 64) {
                //System.out.printf("%5d: %2d => %s\n", count, slot, name);
                INTERFACES[slot][POSITIONS[slot]] = generateInteface(name);
                ++POSITIONS[slot];
                ++count;
            }
        }
        System.out.println(" DONE");

        System.out.print("Executing test cases...");

        int nThreads = Integer.getInteger("WORKERS", Runtime.getRuntime().availableProcessors());

        int TEST_COUNT = Integer.getInteger("TEST_COUNT", Integer.MAX_VALUE);

        BlockingQueue<Runnable> workQueue = new ArrayBlockingQueue<>(nThreads);
        try (var service = new ThreadPoolExecutor(nThreads, nThreads, 0L, TimeUnit.MILLISECONDS, workQueue)) {
            service.prestartAllCoreThreads();

            for (int i = 0; i < TEST_COUNT; i++) {

                final int id = i;
                final Class<?> test = createRandomTest();
                workQueue.put(() -> {
                    long start = System.nanoTime();

                    // Positive test cases
                    for (Class<?> superk : test.getInterfaces()) {
                        run(TEST_MH, test, superk);
                    }

                    // Negative test cases
                    for (int slot = 0; slot < SIZE; slot++) {
                        for (int pos = 0; pos < SIZE; pos++) {
                            Class<?> candidate = INTERFACES[slot][pos];
                            if (!test(test, candidate)) {
                                run(TEST_MH, test, candidate);
                                break; // next slot
                            }
                        }
                    }

                    long finish = System.nanoTime();
                    System.out.printf("%5d: %d supers (finished in %dms)\n", id,
                                      test.getInterfaces().length,
                                      (finish - start) / (1000 * 1000));
                });
            }
        }
    }
}

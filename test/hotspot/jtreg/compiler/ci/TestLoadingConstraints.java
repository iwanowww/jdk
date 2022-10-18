/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

/*
 * @test
 * @bug 8294609
 *
 * @library /test/lib
 *
 * @build compiler.c2.unloaded.TestLoadingConstraints
 *
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar launcher.jar
 *                  compiler.ci.TestLoadingConstraints
 *                  compiler.ci.TestLoadingConstraints$Launcher
 *                  compiler.ci.TestLoadingConstraints$CustomURLLoader
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar caller.jar
 *                  compiler.ci.TestLoadingConstraints$Caller
 *                  compiler.ci.TestLoadingConstraints$Bad
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar callee.jar
 *                  compiler.ci.TestLoadingConstraints$Callee
 *                  compiler.ci.TestLoadingConstraints$Bad
 *
 * @run driver compiler.ci.TestLoadingConstraints
 */

package compiler.ci;

import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.process.OutputAnalyzer;

import java.io.IOException;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.function.Consumer;

public class TestLoadingConstraints {
    static final String THIS_CLASS = TestLoadingConstraints.class.getName();

    static class Bad {}

    public static class Caller {
        static boolean COND = false;
        public static void test() {
            if (COND) {
              Callee.f(null); // effectively unreachable code
            }
        }
    }

    public static class Callee {
        public static void f(Bad b) {}
    }

    static class CustomURLLoader extends URLClassLoader {
        public CustomURLLoader(String name, URL[] urls, ClassLoader parent) {
            super(name, urls, parent);
        }

        protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (name.endsWith("Bad")) {
                Class<?> c = findClass(name);
                if (resolve) {
                    resolveClass(c);
                }
                return c;
            } else {
                return super.loadClass(name, resolve);
            }
        }
    }

    public static class Launcher {
        public static void main(String... args) throws Throwable {
            URLClassLoader calleeCL = new CustomURLLoader("callee", new URL[] { new URL("file:callee.jar") }, ClassLoader.getSystemClassLoader());
            URLClassLoader callerCL = new CustomURLLoader("caller", new URL[] { new URL("file:caller.jar") }, calleeCL);

            Class.forName(THIS_CLASS + "$Callee", false, callerCL);

            Class<?> testClass = Class.forName(THIS_CLASS + "$Caller", false, callerCL);

            MethodHandle testMH = MethodHandles.lookup().findStatic(testClass, "test", MethodType.methodType(void.class));

            for (int i = 0; i < 20_000; i ++) {
                testMH.invokeExact();
            }

            Class<?> badCL1 = Class.forName(THIS_CLASS + "$Bad", false, callerCL); // should succeed
            Class<?> badCL2 = Class.forName(THIS_CLASS + "$Bad", false, calleeCL); // should succeed
            if (badCL1 == badCL2) {
                throw new AssertionError("same");
            }

            System.out.println("TEST PASSED");
        }
    }

    public static void main(String[] args) throws Exception {
        ProcessBuilder pb = new ProcessBuilder();

        pb.command(JDKToolFinder.getJDKTool("java"),
                "-cp", "launcher.jar",
                "-XX:+IgnoreUnrecognizedVMOptions", "-showversion",
                "-XX:-TieredCompilation", "-Xbatch",
                "-XX:+PrintCompilation", "-XX:+UnlockDiagnosticVMOptions", "-XX:+PrintInlining",
                "-XX:CompileCommand=quiet", "-XX:CompileCommand=compileonly,*::test",
                Launcher.class.getName());

        System.out.println("Command line: [" + pb.command() + "]");

        OutputAnalyzer analyzer = new OutputAnalyzer(pb.start());

        analyzer.shouldHaveExitValue(0);

        // The test is applicable only to C2 (present in Server VM).
        analyzer.stderrShouldContain("Server VM");

        analyzer.shouldContain("Caller::test"); // ensure that relevant method is compiled
    }
}

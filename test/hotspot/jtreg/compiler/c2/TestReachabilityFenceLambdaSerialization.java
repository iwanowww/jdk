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

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.io.OutputStream;
import java.io.Serializable;
import java.util.function.Supplier;

/*
 * @test
 * @summary Scalar replacement must preserve the reachability fence control chain with nested resource cleanup
 * @requires vm.compiler2.enabled
 * @run main/othervm -Xcomp -XX:-TieredCompilation -XX:+UnlockDiagnosticVMOptions
 *                   -XX:+StressReachabilityFences TestReachabilityFenceLambdaSerialization
 */
public class TestReachabilityFenceLambdaSerialization {
    interface DefaultAction extends Serializable {
        int value();

        default int ten() {
            return 10;
        }

        default String empty() {
            return "";
        }
    }

    static class Data implements Serializable {
        static Supplier<Data> boundReference() {
            return (Supplier<Data> & Serializable) new Data()::self;
        }

        static Serializable defaultAction(int value) {
            return (DefaultAction) () -> value;
        }

        Data self() {
            return this;
        }
    }

    static void write(OutputStream stream, Serializable object) throws IOException {
        try (ObjectOutputStream out = new ObjectOutputStream(stream)) {
            out.writeObject(object);
        }
    }

    static Object read(InputStream stream) throws IOException, ClassNotFoundException {
        try (ObjectInputStream in = new ObjectInputStream(stream)) {
            return in.readObject();
        }
    }

    static void check(boolean condition) {
        if (!condition) {
            throw new AssertionError();
        }
    }

    static void setup() throws IOException, ClassNotFoundException {
        try (ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            write(out, Data.defaultAction(1));
            byte[] bytes = out.toByteArray();
            try (ByteArrayInputStream in = new ByteArrayInputStream(bytes)) {
                Object object = read(in);
                check(object instanceof DefaultAction);
                check(((DefaultAction) object).value() == 1);
                check(((DefaultAction) object).ten() == 10);
                check(((DefaultAction) object).empty().isEmpty());
            }
        }
    }

    static void test() throws IOException, ClassNotFoundException {
        try (ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            write(out, (Serializable) Data.boundReference());
            byte[] bytes = out.toByteArray();
            try (ByteArrayInputStream in = new ByteArrayInputStream(bytes)) {
                Object object = read(in);
                check(object instanceof Serializable);
                check(object instanceof Supplier);
                check(((Supplier<?>) object).get() instanceof Data);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        // The first round resolves the default-method lambda and serialization paths.
        setup();
        test();
    }
}

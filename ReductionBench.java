package org.openjdk.vector;

import org.openjdk.jmh.annotations.*;

import java.sql.Array;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.Random;

@State(Scope.Thread)
public class ReductionBench {
    static final boolean VERIFY = Boolean.getBoolean("VERIFY");

    static final int SEED = Integer.getInteger("SEED", new Random().nextInt());
    static final Random R = new Random(SEED);

    static int[] initArray(int[] arr) {
        for (int i = 0; i < arr.length; i++) {
            arr[i] = R.nextInt();
        }
        return arr;
    }
    static final int[] RESULTS = new int[100];

    @Setup
    public void setup() {
        a = initArray(new int[size]);
        b = new int[size];
        c = 2;

        if (VERIFY) {
            Arrays.fill(RESULTS, -1);
            RESULTS[0] = testAdd();
            RESULTS[1] = testAddMulConst();
            RESULTS[2] = testAddMulInv();
            RESULTS[3] = testAddMul();
            RESULTS[4] = testMul();
            RESULTS[5] = testMulInv();
            RESULTS[6] = testMulConst();
            RESULTS[7] = testMulAdd();
            RESULTS[8] = testConstMulAdd();
        }
    }


    @Param({1024 + "", 64*1024 + "", 64*1024*1024 + ""})
    public int size;

    int[] a;
    int[] b;

    int c;

    static int VALIDATE(int red, int id) {
        if (VERIFY && RESULTS[id] != -1 && red != RESULTS[id]) {
            throw new AssertionError( RESULTS[id] + " != " + red);
        }
        return red;
    }

    @Benchmark
    public int testAdd() {
        int red = 0;
        for (int i = 0; i < size; i++) {
            red = red + a[i];
        }
        return VALIDATE(red, 0);
    }

    @Benchmark
    public int testAddMulConst() {
        int red = 0;
        for (int i = 0; i < size; i++) {
            red = red + 5 * a[i];
        }
        return VALIDATE(red, 1);
    }

    @Benchmark
    public int testAddMulInv() {
        int red = 0;
        for (int i = 0; i < size; i++) {
            red = red + c * a[i];
        }
        return VALIDATE(red, 2);
    }

    @Benchmark
    public int testAddMul() {
        int red = 0;
        for (int i = 0; i < size; i++) {
            red = red + a[i] * b[i];
        }
        return VALIDATE(red, 3);
    }

    @Benchmark
    public int testMul() {
        int red = 1;
        for (int i = 0; i < size; i++) {
            red = a[i] * red; // YES

            // s = (s * ai) ==> MulReductionV s va
        }
        return VALIDATE(red, 4);
    }

    @Benchmark
    public int testMulInv() {
        int red = 1;
        for (int i = 0; i < size; i++) {
            red = c * red; // NO
        }
        VALIDATE(red, 5);
        return red;
    }

    @Benchmark
    public int testMulConst() {
        int red = 1;
        for (int i = 0; i < size; i++) {
            red = 5 * red; // NO
        }
        return VALIDATE(red, 6);
    }

    @Benchmark
    public int testMulAdd() {
        int red = 1;
        for (int i = 0; i < size; i++) {
            red = a[i] * red + b[i]; // NO

            // s = ((s * ai) + bi) =?=> s = AddReductionV (MulReduction s va) vb =?=> (vs = AddV (MulV vs va) vb) + ???

            // red = (a[i+1] * (a[i+0] * red + *b[i+0]) + b[i+1])
            // red = a[i+1]*a[i+0]*red + a[i+1]*b[i+0] + 1*b[i+1]

            // red3 = a[i+2] * red2 + b[i+2]
            // red  = a[i+2]*a[i+1]*a[i+0]*red + a[i+2]*a[i+1]*b[i+0] + a[i+2]*b[i+1]*1 + b[i+2]

            // [a0  b0] * [r 1]

            // A = [a0 a1]; B = [b0 b1]; R = [r0 r1]
            //

            // [a2*a1*a0      0       0  0] [r]
            // [       0  a2*a1*b0    0  0] [1]
            // [       0      0   a2*b1  0] [1]
            // [       0      0       0 b2] [1]
        }
        return VALIDATE(red, 7);
    }

//    public int testMulAddUnrolled() {
//        int red0 = 1;
//        for (int i = 0; i < size; i++) {
//            int red1 = a[i+0] * red0 + b[i+0];
//            int red2 = a[i+1] * red1 + b[i+1];
//            int red3 = a[i+2] * red2 + b[i+2];
//            int red4 = a[i+3] * red3 + b[i+3];
//
//            red0 = red4;
//        }
//        return red0;
//    }

        // AddReductionVI
        //   (MulI
        //     (AddReductionVI
        //       (MulI Phi ConI)
        //       (MulVI ...))
        //      ConI)
        //   (MulVI ...)

    @Benchmark
    public int testConstMulAdd() {
        int red = 1;
        for (int i = 0; i < size; i++) {
            red = 37 * red + 5 * b[i];
        }
        return VALIDATE(red, 8);
    }
}

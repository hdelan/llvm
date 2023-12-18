// FIXME: the rocm include path and link path are highly platform dependent,
// we should set this with some variable instead
// RUN: %{build} -o %t.out -I/opt/rocm/include -L/opt/rocm/lib -lamdhip64
// RUN: %{run} %t.out
// REQUIRES: hip

#include <iostream>
#include <sycl/sycl.hpp>

#define __HIP_PLATFORM_AMD__

#include <hip/hip_runtime.h>

using namespace sycl;
using namespace sycl::access;

static constexpr size_t BUFFER_SIZE = 1024;

template <typename T> class Modifier;

template <typename T> class Init;

template <typename BufferT, typename ValueT>
void checkBufferValues(BufferT Buffer, ValueT Value) {
  auto Acc = Buffer.get_host_access();
  for (size_t Idx = 0; Idx < Acc.get_count(); ++Idx) {
    if (Acc[Idx] != Value) {
      std::cerr << "buffer[" << Idx << "] = " << Acc[Idx]
                << ", expected val = " << Value << std::endl;
      assert(0 && "Invalid data in the buffer");
    }
  }
}

template <typename DataT>
void copy(buffer<DataT, 1> &Src, buffer<DataT, 1> &Dst, queue &Q) {
  Q.submit([&](handler &CGH) {
    auto SrcA = Src.template get_access<mode::read>(CGH);
    auto DstA = Dst.template get_access<mode::write>(CGH);

    auto Func = [=](interop_handle IH) {
      auto HipStream = IH.get_native_queue<backend::ext_oneapi_hip>();
      auto SrcMem = IH.get_native_mem<backend::ext_oneapi_hip>(SrcA);
      auto DstMem = IH.get_native_mem<backend::ext_oneapi_hip>(DstA);
      cl_event Event;

      if (hipMemcpyWithStream(DstMem, SrcMem, sizeof(DataT) * SrcA.get_count(),
                              hipMemcpyDefault, HipStream) != hipSuccess) {
        throw;
      }

      if (hipStreamSynchronize(HipStream) != hipSuccess) {
        throw;
      }

      if (Q.get_backend() != IH.get_backend())
        throw;
    };
    CGH.host_task(Func);
  });
}

template <typename DataT> void modify(buffer<DataT, 1> &B, queue &Q) {
  Q.submit([&](handler &CGH) {
    auto Acc = B.template get_access<mode::read_write>(CGH);

    auto Kernel = [=](item<1> Id) { Acc[Id] += 1; };

    CGH.parallel_for<Modifier<DataT>>(Acc.get_count(), Kernel);
  });
}

template <typename DataT, DataT B1Init, DataT B2Init>
void init(buffer<DataT, 1> &B1, buffer<DataT, 1> &B2, queue &Q) {
  Q.submit([&](handler &CGH) {
    auto Acc1 = B1.template get_access<mode::write>(CGH);
    auto Acc2 = B2.template get_access<mode::write>(CGH);

    CGH.parallel_for<Init<DataT>>(BUFFER_SIZE, [=](item<1> Id) {
      Acc1[Id] = -1;
      Acc2[Id] = -2;
    });
  });
}

// A test that uses OpenCL interop to copy data from buffer A to buffer B, by
// getting cl_mem objects and calling the clEnqueueBufferCopy. Then run a SYCL
// kernel that modifies the data in place for B, e.g. increment one, then copy
// back to buffer A. Run it on a loop, to ensure the dependencies and the
// reference counting of the objects is not leaked.
void test1(queue &Q) {
  static constexpr int COUNT = 4;
  buffer<int, 1> Buffer1{BUFFER_SIZE};
  buffer<int, 1> Buffer2{BUFFER_SIZE};

  // init the buffer with a'priori invalid data
  init<int, -1, -2>(Buffer1, Buffer2, Q);

  // Repeat a couple of times
  for (size_t Idx = 0; Idx < COUNT; ++Idx) {
    copy(Buffer1, Buffer2, Q);
    modify(Buffer2, Q);
    copy(Buffer2, Buffer1, Q);
  }

  checkBufferValues(Buffer1, COUNT - 1);
  checkBufferValues(Buffer2, COUNT - 1);
}

// Same as above, but performing each command group on a separate SYCL queue
// (on the same or different devices). This ensures the dependency tracking
// works well but also there is no accidental side effects on other queues.
void test2(queue &Q) {
  static constexpr int COUNT = 4;
  buffer<int, 1> Buffer1{BUFFER_SIZE};
  buffer<int, 1> Buffer2{BUFFER_SIZE};

  // init the buffer with a'priori invalid data
  init<int, -1, -2>(Buffer1, Buffer2, Q);

  // Repeat a couple of times
  for (size_t Idx = 0; Idx < COUNT; ++Idx) {
    copy(Buffer1, Buffer2, Q);
    modify(Buffer2, Q);
    copy(Buffer2, Buffer1, Q);
  }
  checkBufferValues(Buffer1, COUNT - 1);
  checkBufferValues(Buffer2, COUNT - 1);
}

// Same as above but with queue constructed out of context
void test2_1(queue &Q) {
  static constexpr int COUNT = 4;
  buffer<int, 1> Buffer1{BUFFER_SIZE};
  buffer<int, 1> Buffer2{BUFFER_SIZE};

  device Device;
  auto Context = context(Device);
  // init the buffer with a'priori invalid data
  init<int, -1, -2>(Buffer1, Buffer2, Q);

  // Repeat a couple of times
  for (size_t Idx = 0; Idx < COUNT; ++Idx) {
    copy(Buffer1, Buffer2, Q);
    modify(Buffer2, Q);
    copy(Buffer2, Buffer1, Q);
  }
  checkBufferValues(Buffer1, COUNT - 1);
  checkBufferValues(Buffer2, COUNT - 1);
}

// Check that a single host-interop-task with a buffer will work
void test3(queue &Q) {
  buffer<int, 1> Buffer{BUFFER_SIZE};

  Q.submit([&](handler &CGH) {
    auto Acc = Buffer.get_access<mode::write>(CGH);
    auto Func = [=](interop_handle IH) { /*A no-op */ };
    CGH.host_task(Func);
  });
}

void test4(queue &Q) {
  buffer<int, 1> Buffer1{BUFFER_SIZE};
  buffer<int, 1> Buffer2{BUFFER_SIZE};

  Q.submit([&](handler &CGH) {
    auto Acc = Buffer1.template get_access<mode::write>(CGH);

    auto Kernel = [=](item<1> Id) { Acc[Id] = 123; };
    CGH.parallel_for<class Test5Init>(Acc.get_count(), Kernel);
  });

  copy(Buffer1, Buffer2, Q);

  checkBufferValues(Buffer2, static_cast<int>(123));
}

void tests(queue &Q) {
  test1(Q);
  test2(Q);
  test2_1(Q);
  test3(Q);
  test4(Q);
}

int main() {
  queue Q([](sycl::exception_list ExceptionList) {
    if (ExceptionList.size() != 1) {
      std::cerr << "Should be one exception in exception list" << std::endl;
      std::abort();
    }
    std::rethrow_exception(*ExceptionList.begin());
  });
  tests(Q);
  tests(Q);
  std::cout << "Test PASSED" << std::endl;
  return 0;
}

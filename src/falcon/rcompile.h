#ifndef RCOMPILE_H_
#define RCOMPILE_H_

#include <cassert>

#include "util.h"
#include "reval.h"
#include "oputil.h"

// While compiling, we use an expanded form to represent opcodes.  This
// is translated to a compact instruction stream as the last compilation
// step.
struct CompilerOp {
  int code;
  int arg;

  // this instruction has been marked dead by an optimization pass,
  // and should be ignored.
  bool dead;

  // is the last register argument a destination we're writing to?
  bool has_dest;

  std::vector<Register> regs;

  std::string str() const;

  CompilerOp(int code, int arg) {
    this->code = code;
    this->arg = arg;
    this->dead = false;
    this->has_dest = false;
  }

  Register dest() {
    size_t n_regs = this->regs.size();
    assert(n_regs > 0);
    assert(this->has_dest);
    return this->regs[n_regs-1];
  }

  size_t num_inputs() {
    size_t n = this->regs.size();
    // if one of the registers is a target for a store, don't count it as an input
    return this->has_dest ? n-1 : n;
  }
};

struct BasicBlock {
private:
  CompilerOp* _add_op(int opcode, int arg, int num_regs);
  CompilerOp* _add_dest_op(int opcode, int arg, int num_regs);

public:

  int py_offset;
  int reg_offset;
  int idx;

  std::vector<BasicBlock*> exits;
  std::vector<BasicBlock*> entries;
  std::vector<CompilerOp*> code;

  // Have we been visited by the current pass already?
  int visited;
  int dead;

  BasicBlock(int offset, int idx);

  /* operations without a destination register */
  CompilerOp* add_op(int opcode, int arg);
  CompilerOp* add_op(int opcode, int arg, int reg1);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4);


  /* operations with a destination register */
  CompilerOp* add_dest_op(int opcode, int arg);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4);

  CompilerOp* add_varargs_op(int opcode, int arg, int num_regs);
};

struct Frame {
  int target;
  int stack_pos;
};

struct RegisterStack {
  int regs[REG_MAX_STACK];
  int stack_pos;

  Frame frames[REG_MAX_FRAMES];
  int num_frames;

  RegisterStack() :
      stack_pos(-1), num_frames(0) {
  }

  void push_frame(int target);
  Frame* pop_frame();

  int push_register(int reg);
  int pop_register();
  int peek_register(int reg);

  std::string str();

};

struct CompilerState {
  std::vector<BasicBlock*> bbs;

  int num_reg;
  int num_consts;
  int num_locals;

  unsigned char* py_codestr;
  Py_ssize_t py_codelen;

  CompilerState() :
      num_reg(0), num_consts(0), num_locals(0), py_codestr(NULL), py_codelen(0) {
  }

  CompilerState(PyCodeObject* code) {
    int codelen = PyString_GET_SIZE(code->co_code);

    num_consts = PyTuple_Size(code->co_consts);
    num_locals = code->co_nlocals;
    // Offset by the number of constants and locals.
    num_reg = num_consts + num_locals;
//    Log_Info("Consts: %d, locals: %d, first register: %d", num_consts, num_locals, num_reg);

    py_codelen = codelen;
    py_codestr = (unsigned char*) PyString_AsString(code->co_code);
  }

  ~CompilerState() {
    for (auto bb : bbs) {
      delete bb;
    }
  }

  BasicBlock* alloc_bb(int offset);
  std::string str();
  void dump(Writer* w);
};

BasicBlock* registerize(CompilerState* state, RegisterStack *stack, int offset);
PyObject* compileByteCode(PyCodeObject* c);
PyObject* compileRegCode(CompilerState* c);

#endif /* RCOMPILE_H_ */
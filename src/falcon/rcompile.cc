#include "Python.h"

#include "Python-ast.h"
#include "node.h"
#include "pyarena.h"
#include "ast.h"
#include "code.h"
#include "compile.h"
#include "symtable.h"
#include "marshal.h"
#include "opcode.h"

#include "rcompile.h"
#include "reval.h"
#include "util.h"

#include <string.h>
#include <stdint.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <string>

#define GETARG(arr, i) ((int)((arr[i+2]<<8) + arr[i+1]))
#define CODESIZE(op)  (HAS_ARG(op) ? 3 : 1)

using namespace std;

std::string CompilerOp::str() const {
  std::string out;
  out += StringPrintf("%s ", OpUtil::name(code));
  if (HAS_ARG(code)) {
    out += StringPrintf("(%d) ", arg);
  }
  out += "[";
  out += StrUtil::join(regs.begin(), regs.end(), ",");
  out += "]";
  if (dead) {
    out += " DEAD ";
  }
  return out;
}

void CompilerState::dump(Writer* w) {
  for (BasicBlock* bb : bbs) {
    w->printf("bb_%d: \n  ", bb->idx);
    w->write(StrUtil::join(bb->code, "\n  "));
    w->write(" -> ");
    w->write(StrUtil::join(bb->exits.begin(), bb->exits.end(), ",", [](BasicBlock* n) {
      return StringPrintf("bb_%d", n->idx);
    }));
    w->write("\n");
  }
}

std::string CompilerState::str() {
  StringWriter w;
  dump(&w);
  return w.str();
}

BasicBlock* CompilerState::alloc_bb(int offset) {
  BasicBlock* bb = new BasicBlock(offset, bbs.size());
  bbs.push_back(bb);
  return bb;
}

std::string RegisterStack::str() {
  return StringPrintf("[%s]", StrUtil::join(&regs[0], &regs[stack_pos], ",", &Coerce::t_str<int>).c_str());
}

CompilerOp* BasicBlock::_add_op(int opcode, int arg, int num_regs) {
  CompilerOp* op = new CompilerOp(opcode, arg);
  op->regs.resize(num_regs);
  code.push_back(op);
  return op;
}

CompilerOp* BasicBlock::_add_dest_op(int opcode, int arg, int num_regs) {
  CompilerOp* op = _add_op(opcode, arg, num_regs);
  op->has_dest = true;
  return op;
}

BasicBlock::BasicBlock(int offset, int idx) {
  reg_offset = 0;
  py_offset = offset;
  visited = 0;
  dead = false;
  this->idx = idx;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg) {
  /* operation with 0 inputs and no destination register */
  return _add_op(opcode, arg, 0);
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1) {
  /* operation with 1 input and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 1);
  op->regs[0] = reg1;
  return op;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1, int reg2) {
  /* operation with 2 inputs and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 2);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  return op;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1, int reg2, int reg3) {
  /* operation with 3 inputs and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 3);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  return op;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4) {
  /* operation with 4 inputs and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 4);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  op->regs[3] = reg4;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg) {
  /* operation with 0 inputs and a destination register */
  return _add_dest_op(opcode, arg, 0);
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1) {
  /* operation with 1 input and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 1);
  op->regs[0] = reg1;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2) {
  /* operation with 2 inputs and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 2);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3) {
  /* operation with 3 inputs and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 3);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4) {
  /* operation with 4 inputs and a destination register */
  CompilerOp* op = _add_op(opcode, arg, 4);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  op->regs[3] = reg4;
  return op;
}

CompilerOp* BasicBlock::add_varargs_op(int opcode, int arg, int num_regs) {
  return _add_dest_op(opcode, arg, num_regs);
}

void RegisterStack::push_frame(int target) {
  assert(num_frames < REG_MAX_FRAMES);
  Frame* f = &frames[num_frames++];
  f->target = target;
  f->stack_pos = stack_pos;
}

Frame* RegisterStack::pop_frame() {
  assert(num_frames > 0);
  Frame* f = &frames[--num_frames];
  stack_pos = f->stack_pos;
  return f;
}

int RegisterStack::push_register(int reg) {
  // Log_Info("Pushing register %d, pos %d", reg, stack_pos + 1);
  assert(stack_pos < REG_MAX_STACK);
  regs[++stack_pos] = reg;
  return reg;
}

int RegisterStack::pop_register() {
  assert(stack_pos >= 0);
  int reg = regs[stack_pos--];
  assert(reg >= -1);
  // Log_Info("Popped register %d, pos: %d", reg, stack_pos + 1);
  return reg;
}

int RegisterStack::peek_register(int reg) {
  return regs[stack_pos - reg];
}

void copy_stack(RegisterStack *from, RegisterStack* to) {
  memcpy(to->regs, from->regs, sizeof(from->regs));
  to->stack_pos = from->stack_pos;
  memcpy(to->frames, from->frames, sizeof(from->frames));
  to->num_frames = from->num_frames;
}

/*
 * The main event: convert from a stack machine to an infinite register machine.
 * We do this using a virtual stack.  Instead of opcodes pushing and popping
 * values from the * stack, we have them push and pop register names.  These
 * register names can then * be used to construct register versions of each
 * opcode.
 *
 * For example, the following operations translate to:
 *
 * LOAD_CONST 1
 * LOAD_CONST 2
 * ADD
 *
 * --->
 *
 * r1 = 1 ('push' r1)
 * r2 = 2 ('push' r2)
 * r3 = add r1, r2 ('pop' r1, r2)
 */
BasicBlock* registerize(CompilerState* state, RegisterStack *stack, int offset) {
  Py_ssize_t r;
  int r1, r2, r3, r4;
  int oparg = 0;
  int opcode = 0;

  unsigned char* codestr = state->py_codestr;

  BasicBlock *last = NULL;
  BasicBlock *entry_point = NULL;

  // If we've already visited this opcode, return the previous block for it.
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock *old = state->bbs[i];
    if (old->py_offset == offset) {
      return old;
    }
  }

  for (; offset < state->py_codelen; offset += CODESIZE(codestr[offset])) {
    opcode = codestr[offset];
    oparg = 0;
    if (HAS_ARG(opcode)) {
      oparg = GETARG(codestr, offset);
    }

    /* The following routines only affect the register stack, and their
     * effect can be captured statically.  We therefore do not have to emit
     * an opcode for them.
     */
    switch (opcode) {
    case NOP:
      continue;
    case ROT_TWO:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r2);
      continue;
    case ROT_THREE:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r3);
      stack->push_register(r2);
      continue;
    default:
      break;
    }

    // Check if the opcode we've advanced to has already been generated.
    // If so, patch ourselves into it and return our entry point.
    for (size_t i = 0; i < state->bbs.size(); ++i) {
      BasicBlock *old = state->bbs[i];
      if (old->py_offset == offset) {
        assert(entry_point != NULL);
        assert(last != NULL);
        last->exits.push_back(old);
        return entry_point;
      }
    }

    BasicBlock *bb = state->alloc_bb(offset);
    if (!entry_point) {
      entry_point = bb;
    }

    if (last) {
      last->exits.push_back(bb);
    }

    last = bb;
    switch (opcode) {
    // Stack pushing/popping

    case NOP:
      break;
    case ROT_TWO:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r2);
      break;
    case ROT_THREE:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r3);
      stack->push_register(r2);
      break;
    case POP_TOP:
      r1 = stack->pop_register();
      bb->add_op(DECREF, 0, r1);
      break;
    case DUP_TOP:
      r1 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r1);
      //bb->add_op(INCREF, 0, r1);
      break;
    case DUP_TOPX:
      if (oparg == 2) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        //bb->add_op(INCREF, 0, r1);
        //bb->add_op(INCREF, 0, r2);
        stack->push_register(r1);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r2);
      } else {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        r3 = stack->pop_register();
        //bb->add_op(INCREF, 0, r1);
        //bb->add_op(INCREF, 0, r2);
        //bb->add_op(INCREF, 0, r3);
        stack->push_register(r3);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r3);
        stack->push_register(r2);
        stack->push_register(r1);
      }
      break;
      // Load operations: push one register onto the stack.
    case LOAD_CONST:
      r1 = oparg;
      r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(LOAD_FAST, 0, r1, r2);
      break;
    case LOAD_FAST:
      r1 = state->num_consts + oparg;
      r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(LOAD_FAST, 0, r1, r2);
      break;
    case LOAD_CLOSURE:
    case LOAD_DEREF:
    case LOAD_GLOBAL:
    case LOAD_LOCALS:
    case LOAD_NAME:
      r1 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1);
      break;
    case LOAD_ATTR:
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
      break;
    case STORE_FAST:
      r1 = stack->pop_register();
      // Decrement the old value.
//      bb->add_op(DECREF, 0, state->num_consts + oparg);
      bb->add_dest_op(opcode, 0, r1, state->num_consts + oparg);
      break;
      // Store operations remove one or more registers from the stack.
    case STORE_DEREF:
    case STORE_GLOBAL:
    case STORE_NAME:
      r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
//      bb->add_op(DECREF, 0, r1);
      break;
    case STORE_ATTR:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
//      bb->add_op(DECREF, 0, r1);
//      bb->add_op(DECREF, 0, r2);
      break;
    case STORE_MAP:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2, r3);
      stack->push_register(r3);
      //bb->add_op(DECREF, 0, r1);
      //bb->add_op(DECREF, 0, r2);
      break;
    case STORE_SUBSCR:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2, r3);
//      bb->add_op(DECREF, 0, r1);
//      bb->add_op(DECREF, 0, r2);
//      bb->add_op(DECREF, 0, r3);
      break;
    case GET_ITER:
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
//      bb->add_op(DECREF, oparg, r1);
      break;
    case SLICE + 0:
    case SLICE + 1:
    case SLICE + 2:
    case SLICE + 3:
      r1 = r2 = r3 = r4 = -1;
      if ((opcode - SLICE) & 2) r3 = stack->pop_register();
      if ((opcode - SLICE) & 1) r2 = stack->pop_register();
      r1 = stack->pop_register();
      r4 = stack->push_register(state->num_reg++);

      if (r2 == -1) {
        bb->add_dest_op(opcode, oparg, r1, r4);
      } else {
        if (r3 == -1) {
          bb->add_dest_op(opcode, oparg, r1, r2, r4);
        } else {
          bb->add_dest_op(opcode, oparg, r1, r2, r3, r4);
        }
      }
      break;
    case STORE_SLICE + 0:
    case STORE_SLICE + 1:
    case STORE_SLICE + 2:
    case STORE_SLICE + 3:
      r1 = r2 = r3 = r4 = -1;
      if ((opcode - STORE_SLICE) & 2) r4 = stack->pop_register();
      if ((opcode - STORE_SLICE) & 1) r3 = stack->pop_register();
      r2 = stack->pop_register();
      r1 = stack->pop_register();
      if (r3 == -1) {
        bb->add_op(opcode, oparg, r1, r2);
      } else {
        if (r4 == -1) {
          bb->add_op(opcode, oparg, r1, r2, r3);
        } else {
          bb->add_op(opcode, oparg, r1, r2, r3, r4);
        }
      }
      break;
    case DELETE_SLICE + 0:
    case DELETE_SLICE + 1:
    case DELETE_SLICE + 2:
    case DELETE_SLICE + 3:
      r1 = r2 = r3 = r4 = -1;
      if ((opcode - DELETE_SLICE) & 2) r4 = stack->pop_register();
      if ((opcode - DELETE_SLICE) & 1) r3 = stack->pop_register();
      r2 = stack->pop_register();
      r1 = stack->pop_register();
      if (r3 == -1) {
        bb->add_op(opcode, oparg, r1, r2);
      } else {
        if (r4 == -1) {
          bb->add_op(opcode, oparg, r1, r2, r3);
        } else {
          bb->add_op(opcode, oparg, r1, r2, r3, r4);
        }
      }
      break;
    case LIST_APPEND:
      r1 = stack->pop_register();
      r2 = stack->peek_register(oparg);
      bb->add_op(opcode, oparg, r1, r2);
      break;
    case UNARY_NOT:
      // Unary operations: pop 1, push 1.
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
      break;
    case UNARY_POSITIVE:
    case UNARY_NEGATIVE:
    case UNARY_CONVERT:
    case UNARY_INVERT:
      // Unary operations: pop 1, push 1.
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
      // bb->add_op(DECREF, 0, r1);
      break;
    case BINARY_POWER:
    case BINARY_MULTIPLY:
    case BINARY_TRUE_DIVIDE:
    case BINARY_FLOOR_DIVIDE:
    case BINARY_MODULO:
    case BINARY_ADD:
    case BINARY_SUBTRACT:
    case BINARY_SUBSCR:
    case BINARY_LSHIFT:
    case BINARY_RSHIFT:
    case BINARY_AND:
    case BINARY_XOR:
    case BINARY_OR:
    case INPLACE_POWER:
    case INPLACE_MULTIPLY:
    case INPLACE_DIVIDE:
    case INPLACE_TRUE_DIVIDE:
    case INPLACE_FLOOR_DIVIDE:
    case INPLACE_MODULO:
    case INPLACE_ADD:
    case INPLACE_SUBTRACT:
    case INPLACE_LSHIFT:
    case INPLACE_RSHIFT:
    case INPLACE_AND:
    case INPLACE_XOR:
    case INPLACE_OR:
    case COMPARE_OP:
      // Binary operations: pop 2, push 1.
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2, r3);
//      bb->add_op(DECREF, 0, r1);
//      bb->add_op(DECREF, 0, r2);
      break;
    case CALL_FUNCTION:
    case CALL_FUNCTION_VAR:
    case CALL_FUNCTION_KW:
    case CALL_FUNCTION_VAR_KW: {
      int na = oparg & 0xff;
      int nk = (oparg >> 8) & 0xff;
      int n = na + 2 * nk;
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, n + 2);
      for (r = n - 1; r >= 0; --r) {
        f->regs[r] = stack->pop_register();
      }
      f->regs[n] = stack->pop_register();
      f->regs[n + 1] = stack->push_register(state->num_reg++);
      assert(f->arg == oparg);

//            for (r = n; r >= 0; --r) { bb->add_op(DECREF, 0, f->regs[r]); }
      break;
    }
    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_TUPLE: {
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, oparg + 1);
      for (r = 0; r < oparg; ++r) {
        f->regs[r] = stack->pop_register();
      }
      f->regs[oparg] = stack->push_register(state->num_reg++);
      break;
    }
    case UNPACK_SEQUENCE: {
      Register seq = stack->pop_register();

      for (r = oparg; r >= 1; --r) {
        Register elt = stack->push_register(state->num_reg++);
        bb->add_dest_op(CONST_INDEX, r - 1, seq, elt);
      }
      break;
    }
//        case SETUP_EXCEPT:
//        case SETUP_FINALLY:
    case SETUP_LOOP:
      stack->push_frame(offset + CODESIZE(state->py_codestr[offset]) + oparg);
      bb->add_op(opcode, oparg);
      break;
    case RAISE_VARARGS:
      r1 = r2 = r3 = -1;
      if (oparg == 3) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        r3 = stack->pop_register();
      } else if (oparg == 2) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
      } else if (oparg == 1) {
        r1 = stack->pop_register();
      }
      bb->add_op(opcode, oparg, r1, r2, r3);
      break;
    case POP_BLOCK:
      stack->pop_frame();
      // bb->add_op(opcode, oparg);
      break;
      // Control flow instructions - recurse down each branch with a copy of the current stack.
    case BREAK_LOOP: {
      Frame *f = stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, f->target));
      if (bb->exits[0] == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case CONTINUE_LOOP: {
      stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, oparg));
      if (bb->exits[0] == NULL) {
        return NULL;
      }
      break;
    }
    case PRINT_ITEM:
      r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      break;
    case PRINT_ITEM_TO:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
      break;
    case PRINT_NEWLINE_TO:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
      break;
    case PRINT_NEWLINE:
      r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      break;
    case FOR_ITER: {
      RegisterStack a, b;
      r1 = stack->pop_register();
      copy_stack(stack, &a);
      copy_stack(stack, &b);
      a.push_register(r1);
      r2 = a.push_register(state->num_reg++);

      bb->add_dest_op(opcode, 0, r1, r2);

      // fall-through if iterator had an item, jump forward if iterator is empty.
      BasicBlock* left = registerize(state, &a, offset + CODESIZE(opcode));
      BasicBlock* right = registerize(state, &b, offset + CODESIZE(opcode) + oparg);
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      if (left == NULL || right == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_TRUE_OR_POP: {
      RegisterStack a, b;
      copy_stack(stack, &a);
      r1 = stack->pop_register();
      copy_stack(stack, &b);
      bb->add_op(opcode, oparg, r1);

      BasicBlock* right = registerize(state, &a, oparg);
      BasicBlock* left = registerize(state, &b, offset + CODESIZE(opcode));
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      if (left == NULL || right == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_TRUE: {
      r1 = stack->pop_register();
      RegisterStack a, b;
      copy_stack(stack, &a);
      copy_stack(stack, &b);
      bb->add_op(opcode, oparg, r1);
      BasicBlock* left = registerize(state, &a, offset + CODESIZE(opcode));
      BasicBlock* right = registerize(state, &b, oparg);
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      if (left == NULL || right == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case JUMP_FORWARD: {
      int dst = oparg + offset + CODESIZE(opcode);
      bb->add_op(JUMP_ABSOLUTE, dst);
      assert(dst <= state->py_codelen);
      BasicBlock* exit = registerize(state, stack, dst);
      bb->exits.push_back(exit);
      if (exit == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case JUMP_ABSOLUTE: {
      bb->add_op(JUMP_ABSOLUTE, oparg);
      BasicBlock* exit = registerize(state, stack, oparg);
      bb->exits.push_back(exit);
      if (exit == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case RETURN_VALUE:
      r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      return entry_point;
    case END_FINALLY:
    case YIELD_VALUE:
    default:
      Log_Info("Unknown opcode %s, arg = %d", OpUtil::name(opcode), oparg);
      return NULL;
      break;
    }
  }
  return entry_point;
}

class CompilerPass {
protected:
  void remove_dead_ops(BasicBlock* bb) {
    size_t live_pos = 0;
    size_t n_ops = bb->code.size();
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];
      if (op->dead) {
        continue;
      }
      bb->code[live_pos++] = op;
    }

    bb->code.resize(live_pos);
  }

  void remove_dead_code(CompilerState* fn) {
    size_t i = 0;
    size_t live_pos = 0;
    size_t n_bbs = fn->bbs.size();
    for (i = 0; i < n_bbs; ++i) {
      BasicBlock* bb = fn->bbs[i];
      if (bb->dead) {
        continue;
      }

      this->remove_dead_ops(bb);
      fn->bbs[live_pos++] = bb;
    }
    fn->bbs.resize(live_pos);
  }
public:
  virtual void visit_op(CompilerOp* op) {
  }

public:
  virtual void visit_bb(BasicBlock* bb) {
    size_t n_ops = bb->code.size();
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp* op = bb->code[i];
      if (!op->dead) {
        this->visit_op(op);
      }
    }
  }

  virtual void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    for (size_t i = 0; i < n_bbs; ++i) {
      BasicBlock* bb = fn->bbs[i];
      if (!bb->visited && !bb->dead) {
        this->visit_bb(bb);
        bb->visited = true;
      }
    }
    this->remove_dead_code(fn);
  }

  void operator()(CompilerState* fn) {
    this->visit_fn(fn);
  }

  virtual ~CompilerPass() {
  }

};

class BackwardPass: public CompilerPass {

public:
  void visit_bb(BasicBlock* bb) {
    size_t n_ops = bb->code.size();
    for (size_t i = n_ops - 1; i-- > 0;) {
      CompilerOp* op = bb->code[i];
      if (!op->dead) {
        this->visit_op(op);
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    for (size_t i = n_bbs - 1; i-- > 0;) {
      BasicBlock* bb = fn->bbs[i];
      if (!bb->visited && !bb->dead) {
        this->visit_bb(bb);
        bb->visited = true;
      }
    }
    this->remove_dead_code(fn);
  }

};

class MarkEntries: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    for (size_t i = 0; i < bb->exits.size(); ++i) {
      BasicBlock* next = bb->exits[i];
      next->entries.push_back(bb);
    }
  }
};

class FuseBasicBlocks: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    if (bb->visited || bb->dead || bb->exits.size() != 1) {
      return;
    }

    BasicBlock* next = bb->exits[0];
    while (1) {
      if (next->entries.size() > 1 || next->visited) {
        break;
      }

      //        Log_Info("Merging %d into %d", next->idx, bb->idx);
      bb->code.insert(bb->code.end(), next->code.begin(), next->code.end());

      next->dead = next->visited = true;
      bb->exits = next->exits;

      if (bb->exits.size() != 1) {
        break;
      }
      next = bb->exits[0];
    }

  }
};

/*

 // For each basic block, find matching increfs and decrefs, and cancel them out.
 void bb_combine_refs(BasicBlock* bb) {
 for (size_t i = 0; i < bb->code.size(); ++i) {
 CompilerOp * decref = bb->code[i];
 if (decref->dead || decref->code != DECREF) {
 continue;
 }
 for (size_t j = 0; j < i; ++j) {
 CompilerOp* incref = bb->code[j];
 if (!incref->dead && incref->code == INCREF && incref->regs[0] == decref->regs[0]) {
 decref->dead = 1;
 incref->dead = 1;
 }
 }
 }
 }
 void opt_combine_refs(CompilerState* state) {
 apply_bb_pass(state, &bb_combine_refs);
 }

 */

class CopyPropagation: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    std::map<Register, Register> env;
    size_t n_ops = bb->code.size();
    Register source, target;
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];

      // check all the registers and forward any that are in the env
      size_t n_inputs = op->num_inputs();
      for (size_t reg_idx = 0; reg_idx < n_inputs; reg_idx++) {
        auto iter = env.find(op->regs[reg_idx]);
        if (iter != env.end()) {
          op->regs[reg_idx] = iter->second;
        }
      }
      if (op->code == LOAD_FAST || op->code == STORE_FAST || op->code == LOAD_CONST) {
        source = op->regs[0];
        target = op->regs[1];
        auto iter = env.find(source);
        if (iter != env.end()) {
          source = iter->second;
        }
        env[target] = source;
      }
    }
  }
};

class UseCounts {
protected:
  std::map<Register, int> counts;

  int get_count(Register r) {
    std::map<Register, int>::iterator iter = counts.find(r);
    return iter == counts.end() ? 0 : iter->second;
  }

  void incr_count(Register r) {
    this->counts[r] = this->get_count(r) + 1;
  }

  void decr_count(Register r) {
    this->counts[r] = this->get_count(r) - 1;
  }

  bool is_pure(int op_code) {
    Log_Debug("Checking if %s is pure\n", OpUtil::name(op_code));
    switch (op_code) {
    case LOAD_LOCALS:
    case LOAD_CONST:
    case LOAD_NAME:
    case BUILD_TUPLE:
    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_MAP:
    case MAKE_CLOSURE:
    case LOAD_GLOBAL:
    case LOAD_FAST:
    case LOAD_DEREF:
    case LOAD_CLOSURE:
    case BUILD_SLICE:
    case CONST_INDEX:
    case STORE_FAST:
      return true;
    default:
      return false;
    }
  }
  void count_uses(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t bb_idx = 0; bb_idx < n_bbs; ++bb_idx) {

      BasicBlock* bb = fn->bbs[bb_idx];
      size_t n_ops = bb->code.size();
      for (size_t op_idx = 0; op_idx < n_ops; ++op_idx) {
        CompilerOp* op = bb->code[op_idx];
        size_t n_inputs = op->num_inputs();

        if (n_inputs > 0) {
          for (size_t reg_idx = 0; reg_idx < n_inputs; reg_idx++) {
            this->incr_count(op->regs[reg_idx]);
          }
        }
      }
    }
  }

};

class StoreElim: public CompilerPass, UseCounts {
public:
  void visit_bb(BasicBlock* bb) {
    // map from registers to their last definition in the basic block
    std::map<Register, CompilerOp*> env;

    // if we encounter a move X->Y when:
    //   - X is locally defined in the basic block
    //   - X is only used once (for this move)
    // then modify the defining instruction of X
    // to directly write to Y and mark the move X->Y as dead

    size_t n_ops = bb->code.size();
    Register source, target;
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];
      // check all the registers and forward any that are in the env
      size_t n_inputs = op->num_inputs();

      if (op->has_dest) {
        target = op->regs[n_inputs];
        env[target] = op;
      }

      if (op->code == LOAD_FAST || op->code == STORE_FAST) {
        source = op->regs[0];
        auto iter = env.find(source);
        if (iter != env.end() && this->get_count(source) == 1) {
          CompilerOp* def = iter->second;
          def->regs[def->num_inputs()] = target;
          op->dead = true;
        }
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->count_uses(fn);
    CompilerPass::visit_fn(fn);
  }
};

class DeadCodeElim: public BackwardPass, UseCounts {
private:

public:
  void visit_op(CompilerOp* op) {
    // printf("visit_op %s (code = %d)\n", op->str().c_str(), op->code);

    size_t n_inputs = op->num_inputs();
    // printf(" -- n_inputs %d\n", n_inputs);
    // printf(" -- has_dest %d\n", op->has_dest);
    // printf(" -- is pure? %d\n", this->is_pure(op->code));
    if ((n_inputs > 0) && (op->has_dest)) {
      Register dest = op->regs[n_inputs];
      // printf(" ** use count %d\n", this->get_count(dest));
      if (this->is_pure(op->code) && this->get_count(dest) == 0) {
        op->dead = true;
        // if an operation is marked dead, decrement the use counts
        // on all of its arguments
        for (size_t input_idx = 0; input_idx < n_inputs; ++input_idx) {
          this->decr_count(op->regs[input_idx]);
        }
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->count_uses(fn);
    Log_Info("%s", Coerce::str(counts).c_str());
    BackwardPass::visit_fn(fn);
  }

};

void optimize(CompilerState* fn) {
  MarkEntries()(fn);
  FuseBasicBlocks()(fn);
  CopyPropagation()(fn);
  StoreElim()(fn);
  DeadCodeElim()(fn);

}

struct RCompilerUtil {
  static int op_size(CompilerOp* op) {
    if (OpUtil::is_varargs(op->code)) {
      return sizeof(RMachineOp) + sizeof(Register) * max(0, (int) op->regs.size() - 2);
    }
    return sizeof(RMachineOp);
  }

  static void write_op(char* dst, CompilerOp* op) {
    RMachineOp* dst_op = (RMachineOp*) dst;
    dst_op->header.code = op->code;
    dst_op->header.arg = op->arg;

    if (OpUtil::is_varargs(op->code)) {
      dst_op->varargs.num_registers = op->regs.size();
      for (size_t i = 0; i < op->regs.size(); ++i) {
        dst_op->varargs.regs[i] = op->regs[i];

        // Guard against overflowing our register size.
        assert(dst_op->varargs.regs[i] == op->regs[i]);
      }

      assert(dst_op->varargs.num_registers == op->regs.size());
    } else if (OpUtil::is_branch(op->code)) {
      assert(op->regs.size() < 3);
      dst_op->branch.reg_1 = op->regs.size() > 0 ? op->regs[0] : -1;
      dst_op->branch.reg_2 = op->regs.size() > 1 ? op->regs[1] : -1;

      // Label be set after the first pass has determined the offset
      // of each instruction.
      dst_op->branch.label = 0;
    } else {
      assert(op->regs.size() <= 3);
      dst_op->reg.reg_1 = op->regs.size() > 0 ? op->regs[0] : -1;
      dst_op->reg.reg_2 = op->regs.size() > 1 ? op->regs[1] : -1;
      dst_op->reg.reg_3 = op->regs.size() > 2 ? op->regs[2] : -1;
    }
  }
};

void lower_register_code(CompilerState* state, std::string *out) {
  RegisterPrelude p;
  memcpy(&p.magic, REG_MAGIC, 4);
  p.mapped_registers = 0;
  p.mapped_labels = 0;
  p.num_registers = state->num_reg;
  out->append((char*) &p, sizeof(RegisterPrelude));

// first, dump all of the operations to the output buffer and record
// their positions.
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    assert(!bb->dead);
    bb->reg_offset = out->size();
    for (size_t j = 0; j < bb->code.size(); ++j) {
      CompilerOp* c = bb->code[j];
      assert(!c->dead);

      size_t offset = out->size();
      out->resize(out->size() + RCompilerUtil::op_size(c));
      RCompilerUtil::write_op(&(*out)[0] + offset, c);
      Log_Debug("Wrote op at offset %d, size: %d, %s", offset, RCompilerUtil::op_size(c), c->str().c_str());
      RMachineOp* rop = (RMachineOp*) (&(*out)[0] + offset);
      Log_AssertEq(RCompilerUtil::op_size(c), RMachineOp::size(*rop));
    }
  }

// now patchup labels in the emitted code to point to the correct
// locations.
  Py_ssize_t pos = sizeof(RegisterPrelude);
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    RMachineOp* op = NULL;

    // Skip to the end of the basic block.
    for (size_t j = 0; j < bb->code.size(); ++j) {
      op = (RMachineOp*) (out->data() + pos);
      Log_Debug("Checking op %s at offset %d.", OpUtil::name(op->code()), pos);

      Log_AssertEq(op->code(), bb->code[j]->code);
      Log_AssertEq(op->arg(), bb->code[j]->arg);
      pos += RMachineOp::size(*op);
    }

    if (OpUtil::is_branch(op->code()) && op->code() != RETURN_VALUE) {
      if (bb->exits.size() == 1) {
        BasicBlock& jmp = *bb->exits[0];
        op->branch.label = jmp.reg_offset;
        Log_AssertGt(jmp.reg_offset, 0);
        Log_AssertEq(op->branch.label, jmp.reg_offset);
      } else {
        // One exit is the fall-through to the next block.
        BasicBlock& a = *bb->exits[0];
        BasicBlock& b = *bb->exits[1];
        BasicBlock& fallthrough = *state->bbs[i + 1];
        Log_Assert(fallthrough.idx == a.idx || fallthrough.idx == b.idx, "One branch must fall-through (%d, %d) != %d",
                   a.idx, b.idx, fallthrough.idx);
        BasicBlock& jmp = (a.idx == fallthrough.idx) ? b : a;
        Log_Info("%d, %d", a.idx, b.idx);
        Log_AssertGt(jmp.reg_offset, 0);
        op->branch.label = jmp.reg_offset;
        Log_AssertEq(op->branch.label, jmp.reg_offset);
      }
    }
  }
}

PyObject* compileRegCode(CompilerState* fn) {

  optimize(fn);

  printf("%s\n", fn->str().c_str());

  std::string regcode;
  lower_register_code(fn, &regcode);
  PyObject* regobj = PyString_FromStringAndSize((char*) regcode.data(), regcode.size());
  return regobj;
}

PyObject* compileByteCode(PyCodeObject* code) {
  CompilerState state(code);
  RegisterStack stack;

  BasicBlock* entry_point = registerize(&state, &stack, 0);
  if (entry_point == NULL) {
    Log_Info("Failed to registerize %s:%d (%s), using stack machine.",
             PyString_AsString(code->co_filename), code->co_firstlineno, PyString_AsString(code->co_name));
    PyErr_SetString(PyExc_SystemError, "Failed to compile function.");
    return NULL;
  }

  return compileRegCode(&state);
}

#!/usr/bin/env python3

"""Common floating-point number operations for f32x4 and f64x2"""

from abc import abstractmethod
import math
import struct


class FloatingPointOp:

    maximum = None

    @abstractmethod
    def binary_op(self, op: str, p1: str, p2: str) -> str:
        pass


class FloatingPointArithOp(FloatingPointOp):
    """Common arithmetic ops for both f32x4 and f64x2:
        neg, sqrt, add, sub, mul, div
    """

    def binary_op(self, op: str, p1: str, p2: str, single_prec=False) -> str:
        """Binary operation on p1 and p2 with the operation specified by op

        :param op: add, sub, mul, div
        :param p1: float number in hex
        :param p2: float number in hex
        :return:
        """
        if '0x' in p1 or '0x' in p2:
            hex_form = True
        else:
            hex_form = False

        if '0x' in p1:
            f1 = float.fromhex(p1)
        else:
            f1 = float(p1)
        if '0x' in p2:
            f2 = float.fromhex(p2)
        else:
            f2 = float(p2)

        if op == 'add':
            if 'inf' in p1 and 'inf' in p2 and p1 != p2:
                return '-nan'
            result = f1 + f2

        elif op == 'sub':
            if 'inf' in p1 and 'inf' in p2 and p1 == p2:
                return '-nan'
            result = f1 - f2

        elif op == 'mul':
            if '0x0p+0' in p1 and 'inf' in p2 or 'inf' in p1 and '0x0p+0' in p2:
                return '-nan'
            if single_prec:
                # For some literals, f32x4.mul operation may cause precision lost.
                # Use struct.unpack('f', struct.pack('f', literal)) to compensate
                # single precision lost of f32
                f1 = struct.unpack('f', struct.pack('f', f1))[0]
                f2 = struct.unpack('f', struct.pack('f', f2))[0]
                result = struct.unpack('f', struct.pack('f', f1 * f2))[0]
            else:
                result = f1 * f2

        elif op == 'div':
            if '0x0p+0' in p1 and '0x0p+0' in p2:
                return '-nan'
            if 'inf' in p1 and 'inf' in p2:
                return '-nan'

            try:
                result = f1 / f2
                return self.get_valid_float(result, self.maximum, hex_form)
            except ZeroDivisionError:
                if p1[0] == p2[0]:
                    return 'inf'
                elif p1 == 'inf' and p2 == '0x0p+0':
                    return 'inf'
                else:
                    return '-inf'

        else:
            raise Exception('Unknown binary operation')

        return self.get_valid_float(result, self.maximum, hex_form)

    def get_valid_float(self, value, maximum_literals, hex_form=False):
        if value > float.fromhex(maximum_literals):
            return 'inf'
        if value < float.fromhex('-' + maximum_literals):
            return '-inf'

        if hex_form:
            return value.hex()
        else:
            return str(value)

    def float_sqrt(self, p):
        if p == '-0x0p+0':
            return '-0x0p+0'

        try:
            if '0x' in p:
                f = float.fromhex(p)
                result = float.hex(math.sqrt(f))
            else:
                f = float(p)
                result = str(math.sqrt(f))
        except ValueError:
            result = '-nan'

        return result

    def float_neg(self, p):
        if p == 'nan':
            return '-nan'
        try:
            if '0x' in p:
                f = float.fromhex(p)
                result = float.hex(-f)
            else:
                f = float(p)
                result = str(-f)
        except ValueError:
            if p.startswith('nan:'):
                return '-' + p
            if p.startswith('-nan:'):
                return p[1:]

        return result


class FloatingPointSimpleOp(FloatingPointOp):
    """Common simple ops for both f32x4 and f64x2: abs, min, max"""

    def binary_op(self, op: str, p1: str, p2: str) -> str:
        """Binary operation on p1 and p2 with the operation specified by op

        :param op: min, max,
        :param p1: float number in hex
        :param p2: float number in hex
        :return:
        """
        f1 = float.fromhex(p1)
        f2 = float.fromhex(p2)

        if '-nan' in [p1, p2] and 'nan' in [p1, p2]:
            return p1

        if 'nan' in [p1, p2]:
            return 'nan'

        if '-nan' in [p1, p2]:
            return '-nan'

        if op == 'min':
            if '-0x0p+0' in [p1, p2] and '0x0p+0' in [p1, p2]:
                return '-0x0p+0'
            result = min(f1, f2)

        elif op == 'max':
            if '-0x0p+0' in [p1, p2] and '0x0p+0' in [p1, p2]:
                return '0x0p+0'
            result = max(f1, f2)

        else:
            raise Exception('Unknown binary operation: {}'.format(op))

        return result.hex()

    def unary_op(self, op: str, p1: str) -> str:
        """Unnary operation on p1 with the operation specified by op

        :param op: abs,
        :param p1: float number in hex
        :return:
        """
        f1 = float.fromhex(p1)
        if op == 'abs':
            return abs(f1).hex()

        raise Exception('Unknown unary operation: {}'.format(op))


class FloatingPointCmpOp(FloatingPointOp):

    def binary_op(self, op: str, p1: str, p2: str) -> str:
        """Binary operation on p1 and p2 with the operation specified by op

        :param op: eq, ne, lt, le, gt, ge
        :param p1: float number in hex
        :param p2: float number in hex
        :return:
        """

        # ne
        # if either p1 or p2 is a NaN, then return True
        if op == 'ne' and ('nan' in p1.lower() or 'nan' in p2.lower()):
            return '-1'

        # other instructions
        # if either p1 or p2 is a NaN, then return False
        if 'nan' in p1.lower() or 'nan' in p2.lower():
            return '0'

        f1 = float.fromhex(p1)
        f2 = float.fromhex(p2)

        if op == 'eq':
            return '-1' if f1 == f2 else '0'

        elif op == 'ne':
            return '-1' if f1 != f2 else '0'

        elif op == 'lt':
            return '-1' if f1 < f2 else '0'

        elif op == 'le':
            return '-1' if f1 <= f2 else '0'

        elif op == 'gt':
            return '-1' if f1 > f2 else '0'

        elif op == 'ge':
            return '-1' if f1 >= f2 else '0'
        else:
            raise Exception('Unknown binary operation')
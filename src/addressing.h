//
// Yosys slang frontend
//
// Copyright 2024 Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
#pragma once
namespace slang_frontend {

// TODO: audit for overflows
struct Addressing {
	const ast::Expression &expr;

	SignalEvalContext &eval;
	NetlistContext &netlist;
	slang::ConstantRange range;

	using Signal = RTLIL::SigSpec;
	const RTLIL::State S0 = RTLIL::S0;
	const RTLIL::State S1 = RTLIL::S1;
	const RTLIL::State Sx = RTLIL::Sx;

	// these summed together are the zero-based index of the bottom item
	// of the selection
	Signal raw_signal;
	int base_offset;

	int stride = 1;

	void interpret_index(Signal signal, int width_down=1, int width_up=1)
	{
		if (range.isLittleEndian()) {
			base_offset = -range.right - width_down + 1;
			raw_signal = signal;
		} else {
			base_offset = range.right - width_up + 1;

			// We might want some other handling of big-endian
			// indexing.
			raw_signal = netlist.Not(signal);
			base_offset += 1;
		}
	}

	Addressing(SignalEvalContext &eval, const ast::ElementSelectExpression &sel)
		: expr(sel), eval(eval), netlist(eval.netlist)
	{
		require(sel, sel.value().type->hasFixedRange());
		range = sel.value().type->getFixedRange();
		interpret_index(eval.eval_signed(sel.selector()));

		stride = sel.type->getBitstreamWidth();
	}

	Addressing(SignalEvalContext &eval, const ast::RangeSelectExpression &sel)
		: expr(sel), eval(eval), netlist(eval.netlist)
	{
		require(sel, sel.value().type->hasFixedRange());
		range = sel.value().type->getFixedRange();

		switch (sel.getSelectionKind()) {
		case ast::RangeSelectionKind::Simple:
			{
				require(sel, sel.left().constant && sel.right().constant);
				raw_signal = {S0};

				int right_sel = sel.right().constant->integer().as<int>().value();

				if (range.isLittleEndian())
					base_offset = right_sel - range.right;
				else
					base_offset = range.right - right_sel;
			}
			break;
		case ast::RangeSelectionKind::IndexedUp:
			{
				Signal signal = eval.eval_signed(sel.left());
				require(sel, sel.right().constant);
				int right_sel = sel.right().constant->integer().as<int>().value();
				interpret_index(signal, 1, right_sel);
			}
			break;
		case ast::RangeSelectionKind::IndexedDown:
			{
				Signal signal = eval.eval_signed(sel.left());
				require(sel, sel.right().constant);
				int right_sel = sel.right().constant->integer().as<int>().value();
				interpret_index(signal, right_sel, 1);
			}
			break;
		}

		if (sel.value().type->isArray())
			stride = sel.value().type->getArrayElementType()->getBitstreamWidth();
		else
			stride = 1;
	}

	Signal shift_up(Signal val, bool oor_undef, int output_len)
	{
		log_assert(stride == 1);
		int shifted_len = output_len;
		Signal val2 = val, shifted;

		if (base_offset > 0) {
			Signal padding(oor_undef ? RTLIL::Sx : S0, base_offset);
			val2 = {val, padding};
		} else if (base_offset < 0) {
			shifted_len += -base_offset;
		}

		if (oor_undef)
			shifted = netlist.Shiftx(val2, netlist.Neg(raw_signal, true), true, shifted_len);
		else
			shifted = netlist.Shift(val2, false, netlist.Neg(raw_signal, true), true, shifted_len);

		if (base_offset < 0)
			return shifted.extract_end(-base_offset);
		else
			return shifted;
	}

	Signal raw_demux(Signal val, int from, int to)
	{
		log_assert(val.size() == stride);
		Signal negative, positive;

		if (from < 0) {
			// Build the negative branch
			int demux_size = std::bit_ceil((unsigned int) -from);
			int sel_size = ceil_log2(demux_size);

			Signal sel = raw_signal;
			sel.extend_u0(sel_size, true);

			// check `raw_signal` is in between -2**sel_size...0
			// which is where the demuxing is valid
			Signal valid =
				netlist.LogicAnd(
					netlist.Ge(raw_signal, {S1, Signal(S0, sel_size)}, true),
					netlist.Lt(raw_signal, {S0}, true));

			Signal val_gated =
				netlist.Mux(Signal(S0, stride), val, valid);

			negative = netlist.Demux(val_gated, sel).extract_end((stride << sel_size) + from * stride);
			log_assert(negative.size() == -from * stride);
		}

		if (to > 0) {
			// Build the nonnegative branch
			int demux_size = std::bit_ceil((unsigned int) to);
			int sel_size = ceil_log2(demux_size);

			Signal sel = raw_signal;
			sel.extend_u0(sel_size, true);

			// check `raw_signal` is in between 0...2**sel_size
			// which is where the demuxing is valid
			Signal valid =
				netlist.LogicAnd(
					netlist.Ge(raw_signal, {S0}, true),
					netlist.Lt(raw_signal, {S0, S1, Signal(S0, sel_size)}, true));

			Signal val_gated =
				netlist.Mux(Signal(S0, stride), val, valid);

			Signal demux_result = netlist.Demux(val_gated, sel);

			positive = netlist.Demux(val_gated, sel).extract(0, to * stride);
			log_assert(positive.size() == to * stride);
		}

		return {positive, negative};
	}

	Signal demux(Signal val, int output_len)
	{
		log_assert(val.size() == stride);
		log_assert(output_len % stride == 0);
		Signal demuxed = raw_demux(val,
										-std::max(0, base_offset),
										std::max(0, output_len / stride - base_offset));

		return demuxed.extract(std::max(0, -stride * base_offset), output_len);
	}

	Signal raw_mux(Signal val, int from, int to, int stride)
	{
		log_assert(stride * (to - from) == val.size());
		Signal negative(Sx, stride), positive(Sx, stride);

		if (from < 0) {
			// Build the negative branch
			int mux_size = std::bit_ceil((unsigned int) -from);
			int sel_size = ceil_log2(mux_size);

			Signal val_cut = val.extract(0, -from * stride);
			Signal val_padded = {val_cut, Signal(Sx, (1 << sel_size) * stride - val_cut.size())};

			Signal sel = raw_signal;
			sel.extend_u0(sel_size, true);
			Signal valid = netlist.Ge(raw_signal, {S1, Signal(S0, sel_size)}, true);
			negative = netlist.Mux(Signal(Sx, stride), netlist.Bmux(val_padded, sel), valid);
		}

		if (to > 0) {
			// Build the positive branch
			int mux_size = std::bit_ceil((unsigned int) to);
			int sel_size = ceil_log2(mux_size);

			Signal val_cut = val.extract_end(-from * stride);
			Signal val_padded = {Signal(Sx, (1 << sel_size) * stride - val_cut.size()), val_cut};

			Signal sel = raw_signal;
			sel.extend_u0(sel_size, true);
			Signal valid = netlist.Lt(raw_signal, {S0, S1, Signal(S0, sel_size)}, true);

			positive = netlist.Mux(Signal(Sx, stride), netlist.Bmux(val_padded, sel), valid);
		}

		return netlist.Mux(positive, negative, raw_signal.msb());
	}

	Signal mux(Signal val, int output_len)
	{
		log_assert(output_len == stride);
		log_assert(val.size() % stride == 0);
		return raw_mux(
			{Signal(Sx, std::max(0, base_offset * stride - val.size())),
			 val,
			 Signal(Sx, std::max(0, stride * -base_offset))},
			-std::max(0, base_offset),
			std::max(0, -base_offset + val.size() / stride), output_len);
	}

	Signal shift_down(Signal val, int output_len)
	{
		log_assert(stride == 1);
		int shifted_len = output_len;
		Signal val2 = val, shifted;

		if (base_offset > 0)
			shifted_len += base_offset;
		else if (base_offset < 0)
			val2 = {val, Signal(RTLIL::Sx, -base_offset)};

		shifted = netlist.Shiftx(val2, raw_signal, true, shifted_len);
		
		if (base_offset > 0)
			return shifted.extract_end(base_offset);
		else
			return shifted;
	}

	Signal extract(Signal val, int width)
	{
		require(expr, raw_signal.is_fully_def());
		int offset = raw_signal.as_const().as_int(true) + base_offset;
		require(expr, offset >= 0 && offset * stride + width <= val.size());
		return val.extract(offset * stride, width);
	}

	Signal embed(Signal val, int output_len, int stride, RTLIL::State padding)
	{
		require(expr, raw_signal.is_fully_def());
		int offset = raw_signal.as_const().as_int(true) + base_offset;
		require(expr, offset >= 0 && offset * stride + val.size() <= output_len);
		return {Signal(padding, output_len - offset * stride - val.size()), val, {Signal(padding, offset * stride)}};
	}
};

};

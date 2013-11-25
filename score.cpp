#include "assert.hpp"
#include "eval_values.hpp"
#include "score.hpp"


score::score()
	: mg_()
	, eg_()
{
}


score::score( value_type mg, value_type eg )
	: mg_(mg)
	, eg_(eg)
{
}


score::score( score const& rhs )
	: mg_(rhs.mg_)
	, eg_(rhs.eg_)
{
}


score& score::operator=( score const& rhs )
{
	mg_ = rhs.mg_;
	eg_ = rhs.eg_;

	return *this;
}


short score::scale( value_type material ) const
{
	if( material >= eval_values::phase_transition_material_begin ) {
		return mg_;
	}
	else if( material <= eval_values::phase_transition_material_end ) {
		return eg_;
	}
	
	int position = 256 * (eval_values::phase_transition_material_begin - material) / static_cast<int>(eval_values::phase_transition_duration);
	int v = ((static_cast<int>(eg_) * position      )) +
		    ((static_cast<int>(mg_) * (256-position)));

	return static_cast<short>(v / 256);
}


score score::operator+( score const& rhs ) const
{
	score ret = *this;
	ret += rhs;
	return ret;
}


score score::operator-( score const& rhs ) const
{
	score ret = *this;
	ret -= rhs;
	return ret;
}


score score::operator-() const
{
	return score( -mg_, -eg_ );
}


score& score::operator+=( score const& rhs )
{
	ASSERT( static_cast<int>(mg_) + rhs.mg_ < 32768 );
	ASSERT( static_cast<int>(mg_) + rhs.mg_ >= -32768 );
	ASSERT( static_cast<int>(eg_) + rhs.eg_ < 32768 );
	ASSERT( static_cast<int>(eg_) + rhs.eg_ >= -32768 );

	mg_ += rhs.mg_;
	eg_ += rhs.eg_;

	return *this;
}


score& score::operator-=( score const& rhs )
{
	ASSERT( static_cast<int>(mg_) - rhs.mg_ < 32768 );
	ASSERT( static_cast<int>(mg_) - rhs.mg_ >= -32768 );
	ASSERT( static_cast<int>(eg_) - rhs.eg_ < 32768 );
	ASSERT( static_cast<int>(eg_) - rhs.eg_ >= -32768 );

	mg_ -= rhs.mg_;
	eg_ -= rhs.eg_;

	return *this;
}


bool score::operator==( score const& rhs ) const {
	return mg_ == rhs.mg_ && eg_ == rhs.eg_;
}


bool score::operator!=( score const& rhs ) const {
	return !(*this == rhs);
}


std::ostream& operator<<(std::ostream& stream, score const& s)
{
	return stream << s.mg() << " " << s.eg();
}


score score::operator*( value_type m ) const
{
	ASSERT( static_cast<int>(mg_) * m < 32768 );
	ASSERT( static_cast<int>(mg_) * m >= -32768 );
	ASSERT( static_cast<int>(eg_) * m < 32768 );
	ASSERT( static_cast<int>(eg_) * m >= -32768 );

	return score( mg_ * m, eg_ * m );
}


score score::operator*( score const& m ) const
{
	ASSERT( static_cast<int>(mg_) * m.mg_ < 32768 );
	ASSERT( static_cast<int>(mg_) * m.mg_ >= -32768 );
	ASSERT( static_cast<int>(eg_) * m.eg_< 32768 );
	ASSERT( static_cast<int>(eg_) * m.eg_ >= -32768 );

	return score( mg_ * m.mg_, eg_ * m.eg_ );
}


score& score::operator*=( value_type m )
{
	ASSERT( static_cast<int>(mg_) * m < 32768 );
	ASSERT( static_cast<int>(mg_) * m >= -32768 );
	ASSERT( static_cast<int>(eg_) * m < 32768 );
	ASSERT( static_cast<int>(eg_) * m >= -32768 );

	mg_ *= m;
	eg_ *= m;

	return *this;
}


score score::operator/( value_type m ) const
{
	ASSERT( m != 0 );
	return score( mg_ / m, eg_ / m );
}


score score::operator/( score const& m ) const
{
	ASSERT( m.mg_ != 0 );
	ASSERT( m.eg_ != 0 );
	return score( mg_ / m.mg_, eg_ / m.eg_ );
}

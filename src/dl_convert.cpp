/* copyright (c) 2010 Fredrik Kihlander, see LICENSE for more info */

#include <dl/dl.h>
#include "dl_types.h"
#include "dl_binary_writer.h"
#include "container/dl_array.h"

struct SInstance
{
	SInstance() {}
	SInstance(const uint8* _pAddress, const SDLType* _pType, pint _ArrayCount, EDLType _Type)
		: m_pAddress(_pAddress)
		, m_ArrayCount(_ArrayCount)
		, m_pType(_pType)
		, m_Type(_Type)
		{ }

	const uint8*   m_pAddress;
	pint           m_ArrayCount;
	pint           m_OffsetAfterPatch;
	const SDLType* m_pType;
	EDLType        m_Type;
};

class SConvertContext
{
public:
	SConvertContext( ECpuEndian _SourceEndian, ECpuEndian _TargetEndian, EDLPtrSize _SourcePtrSize, EDLPtrSize _TargetPtrSize )
		: m_SourceEndian(_SourceEndian)
		, m_TargetEndian(_TargetEndian)
		, m_SourcePtrSize(_SourcePtrSize)
		, m_TargetPtrSize(_TargetPtrSize)
	{}

	bool IsSwapped(const uint8* _Ptr)
	{
		for(uint iInstance = 0; iInstance < m_lInstances.Len(); ++iInstance)
			if(_Ptr == m_lInstances[iInstance].m_pAddress)
				return true;
		return false;
	}

	ECpuEndian m_SourceEndian;
	ECpuEndian m_TargetEndian;
	EDLPtrSize m_SourcePtrSize;
	EDLPtrSize m_TargetPtrSize;

	CArrayStatic<SInstance, 128> m_lInstances;

	struct PatchPos
	{
		PatchPos() {}
		PatchPos(pint _Pos, pint _OldOffset)
			: m_Pos(_Pos)
			, m_OldOffset(_OldOffset)
		{ }
		pint m_Pos;
		pint m_OldOffset;
	};

	CArrayStatic<PatchPos, 256> m_lPatchOffset;
};

static inline void DLSwapHeader(SDLDataHeader* _pHeader)
{
	_pHeader->m_Id               = DLSwapEndian(_pHeader->m_Id);
	_pHeader->m_Version          = DLSwapEndian(_pHeader->m_Version);
	_pHeader->m_RootInstanceType = DLSwapEndian(_pHeader->m_RootInstanceType);
	_pHeader->m_InstanceSize     = DLSwapEndian(_pHeader->m_InstanceSize);
}

static pint DLInternalReadPtrData( const uint8* _pPtrData, 
								   ECpuEndian   _SourceEndian,
								   EDLPtrSize   _PtrSize )
{
	switch(_PtrSize)
	{
		case DL_PTR_SIZE_32BIT: 
		{
			uint32 Offset = *(uint32*)_pPtrData; 

			if(_SourceEndian != ENDIAN_HOST)
				return (pint)DLSwapEndian(Offset);
			else
				return (pint)Offset;
		}
		break;
		case DL_PTR_SIZE_64BIT: 
		{
			uint64 Offset = *(uint64*)_pPtrData; 

			if(_SourceEndian != ENDIAN_HOST)
				return (pint)DLSwapEndian(Offset);
			else
				return (pint)Offset;
		}
		break;
		default:
			M_ASSERT(false && "Invalid ptr-size");
	}

	return 0;
}

static void DLInternalReadArrayData( const uint8* _pArrayData, 
									 pint*        _pOffset, 
									 uint32*      _pCount, 
									 ECpuEndian   _SourceEndian,
									 EDLPtrSize   _PtrSize )
{
	union { const uint8* m_u8; const uint32* m_u32; const uint64* m_u64; } pArrayData;
	pArrayData.m_u8 = _pArrayData;

	switch(_PtrSize)
	{
		case DL_PTR_SIZE_32BIT:
		{
			uint32 Offset = pArrayData.m_u32[0];
			uint32 Count  = pArrayData.m_u32[1];

			if(_SourceEndian != ENDIAN_HOST)
			{
				*_pOffset = (pint)DLSwapEndian(Offset);
				*_pCount  = DLSwapEndian(Count);
			}
			else
			{
				*_pOffset = (pint)(Offset);
				*_pCount  = Count;
			}
		}
		break;

		case DL_PTR_SIZE_64BIT:
		{
			uint64 Offset = pArrayData.m_u64[0];
			uint32 Count  = pArrayData.m_u32[2];

			if(_SourceEndian != ENDIAN_HOST)
			{
				*_pOffset = (pint)DLSwapEndian(Offset);
				*_pCount  = DLSwapEndian(Count);
			}
			else
			{
				*_pOffset = (pint)(Offset);
				*_pCount  = Count;
			}
		}
		break;

		default:
			M_ASSERT(false && "Invalid ptr-size");
	}
}

static EDLError DLInternalConvertCollectInstances( HDLContext       _Context, 
												   const SDLType*   _pType, 
												   const uint8*     _pData, 
												   const uint8*     _pBaseData,
												   SConvertContext& _ConvertContext )
{
	for(uint32 iMember = 0; iMember < _pType->m_nMembers; ++iMember)
	{
		const SDLMember& Member = _pType->m_lMembers[iMember];
		const uint8* pMemberData = _pData + Member.m_Offset[_ConvertContext.m_SourcePtrSize];

		EDLType AtomType    = Member.AtomType();
		EDLType StorageType = Member.StorageType();

		switch(AtomType)
		{
			case DL_TYPE_ATOM_POD:
			{
				if(StorageType == DL_TYPE_STORAGE_STR)
				{
					pint Offset = DLInternalReadPtrData( pMemberData, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize );
					_ConvertContext.m_lInstances.Add(SInstance(_pBaseData + Offset, 0x0, 1337, Member.m_Type));
				}
				else if( StorageType == DL_TYPE_STORAGE_PTR )
				{
					const SDLType* pSubType = DLFindType(_Context, Member.m_TypeID);
					if(pSubType == 0x0)
						return DL_ERROR_TYPE_NOT_FOUND;

					pint Offset = DLInternalReadPtrData(pMemberData, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);

					const uint8* pPtrData = _pBaseData + Offset;

					if(Offset != DL_NULL_PTR_OFFSET[_ConvertContext.m_SourcePtrSize] && !_ConvertContext.IsSwapped(pPtrData))
					{
						_ConvertContext.m_lInstances.Add(SInstance(pPtrData, pSubType, 0, Member.m_Type));
						DLInternalConvertCollectInstances(_Context, pSubType, _pBaseData + Offset, _pBaseData, _ConvertContext);
					}
				}
				break;
			}
			break;
			case DL_TYPE_ATOM_INLINE_ARRAY:
			{
				switch(StorageType)
				{
					case DL_TYPE_STORAGE_STRUCT:
					{
						const SDLType* pSubType = DLFindType(_Context, Member.m_TypeID);
						if(pSubType == 0x0)
							return DL_ERROR_TYPE_NOT_FOUND;

						for (pint ElemOffset = 0; ElemOffset < Member.m_Size[_ConvertContext.m_SourcePtrSize]; ElemOffset += pSubType->m_Size[_ConvertContext.m_SourcePtrSize])
							DLInternalConvertCollectInstances(_Context, pSubType, pMemberData + ElemOffset, _pBaseData, _ConvertContext);
					}
					break;
					case DL_TYPE_STORAGE_STR:
					{
						// TODO: This might be optimized if we look at all the data in i inline-array of strings as 1 instance continious in memory.
						// I am not sure if that is true for all cases right now!

						uint32 PtrSize = (uint32)DLPtrSize(_ConvertContext.m_SourcePtrSize);
						uint32 Count = Member.m_Size[_ConvertContext.m_SourcePtrSize] / PtrSize;

						for (pint iElem = 0; iElem < Count; ++iElem)
						{
							pint Offset = DLInternalReadPtrData(_pData + (iElem * PtrSize), _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);
							_ConvertContext.m_lInstances.Add(SInstance(_pBaseData + Offset, 0x0, Count, EDLType(DL_TYPE_ATOM_POD | DL_TYPE_STORAGE_STR)));
						}
					}
					break;
					default:
						M_ASSERT(Member.IsSimplePod() || StorageType == DL_TYPE_STORAGE_ENUM);
						// ignore
				}
			}
			break;

			case DL_TYPE_ATOM_ARRAY:
			{
				pint Offset = 0; uint32 Count = 0;
				DLInternalReadArrayData( pMemberData, &Offset, &Count, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize );

				if(Offset == DL_NULL_PTR_OFFSET[_ConvertContext.m_SourcePtrSize])
					break;
					
				switch(StorageType)
				{
					case DL_TYPE_STORAGE_STR:
					{
						// TODO: This might be optimized if we look at all the data in i inline-array of strings as 1 instance continious in memory.
						// I am not sure if that is true for all cases right now!

						uint32 PtrSize = (uint32)DLPtrSize(_ConvertContext.m_SourcePtrSize);
						const uint8* pArrayData = _pBaseData + Offset;

						_ConvertContext.m_lInstances.Add(SInstance(_pBaseData + Offset, 0x0, Count, Member.m_Type));

						for (pint iElem = 0; iElem < Count; ++iElem)
						{
							pint ElemOffset = DLInternalReadPtrData(pArrayData + (iElem * PtrSize), _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);
							_ConvertContext.m_lInstances.Add(SInstance(_pBaseData + ElemOffset, 0x0, Count, EDLType(DL_TYPE_ATOM_POD | DL_TYPE_STORAGE_STR)));
						}
					}
					break;

					case DL_TYPE_STORAGE_STRUCT:
					{
						DLInternalReadArrayData( pMemberData, &Offset, &Count, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize );

						const uint8* pArrayData = _pBaseData + Offset;

						const SDLType* pSubType = DLFindType(_Context, Member.m_TypeID);
						if(pSubType == 0x0)
							return DL_ERROR_TYPE_NOT_FOUND;

						_ConvertContext.m_lInstances.Add(SInstance(pArrayData, pSubType, Count, Member.m_Type));

						for (pint ElemOffset = 0; ElemOffset < pSubType->m_Size[_ConvertContext.m_SourcePtrSize] * Count; ElemOffset += pSubType->m_Size[_ConvertContext.m_SourcePtrSize])
							DLInternalConvertCollectInstances(_Context, pSubType, pArrayData + ElemOffset, _pBaseData, _ConvertContext);
					}
					break;

					default:
					{
						M_ASSERT(Member.IsSimplePod() || StorageType == DL_TYPE_STORAGE_ENUM);
						DLInternalReadArrayData( pMemberData, &Offset, &Count, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize );
						_ConvertContext.m_lInstances.Add(SInstance(_pBaseData + Offset, 0x0, Count, Member.m_Type));
					}
					break;
				}
			}
			break;

			case DL_TYPE_ATOM_BITFIELD:
				// ignore
				break;

			default:
				M_ASSERT(false && "Invalid ATOM-type!");
		}
	}

	return DL_ERROR_OK;
}

template<typename T>
static T DLConvertBitFieldFormat(T _OldValue, const SDLMember* _plBFMember, uint32 _nBFMembers, SConvertContext& _ConvertContext)
{
	if(_ConvertContext.m_SourceEndian != ENDIAN_HOST)
		_OldValue = DLSwapEndian(_OldValue);

	T NewValue = 0;

	for( uint32 iBFMember = 0; iBFMember < _nBFMembers; ++iBFMember)
	{
		const SDLMember& BFMember = _plBFMember[iBFMember];

		uint32 BFBits   = BFMember.BitFieldBits();
		uint32 BFOffset = BFMember.BitFieldOffset();
		uint32 BFSourceOffset = DLBitFieldOffset(_ConvertContext.m_SourceEndian, sizeof(T), BFOffset, BFBits);
		uint32 BFTargetOffset = DLBitFieldOffset(_ConvertContext.m_TargetEndian, sizeof(T), BFOffset, BFBits);

 		T Extracted = DL_EXTRACT_BITS(_OldValue, T(BFSourceOffset), T(BFBits));
 		NewValue    = DL_INSERT_BITS(NewValue, Extracted, T(BFTargetOffset), T(BFBits));
	}

	if(_ConvertContext.m_SourceEndian != ENDIAN_HOST)
		return DLSwapEndian(NewValue);

	return NewValue;
}

static EDLError DLInternalConvertWriteStruct( HDLContext       _Context, 
											  const uint8*     _pData,  
											  const SDLType*   _pType,
											  SConvertContext& _ConvertContext,
											  CDLBinaryWriter& _Writer )
{
	_Writer.Align(_pType->m_Alignment[_ConvertContext.m_TargetPtrSize]);
	pint Pos = _Writer.Tell();
	_Writer.Reserve(_pType->m_Size[_ConvertContext.m_TargetPtrSize]);

	for(uint32 iMember = 0; iMember < _pType->m_nMembers; ++iMember)
	{
		const SDLMember& Member  = _pType->m_lMembers[iMember];
		const uint8* pMemberData = _pData + Member.m_Offset[_ConvertContext.m_SourcePtrSize];

		_Writer.Align(Member.m_Alignment[_ConvertContext.m_TargetPtrSize]);

		EDLType AtomType    = Member.AtomType();
		EDLType StorageType = Member.StorageType();

		switch(AtomType)
		{
			case DL_TYPE_ATOM_POD:
			{
				switch (StorageType)
				{
					case DL_TYPE_STORAGE_STRUCT:
					{
						const SDLType* pSubType = DLFindType(_Context, Member.m_TypeID);
						if(pSubType == 0x0)
							return DL_ERROR_TYPE_NOT_FOUND;
						DLInternalConvertWriteStruct(_Context, pMemberData, pSubType, _ConvertContext, _Writer);
					}
					break;
					case DL_TYPE_STORAGE_STR:
					{
						pint Offset = DLInternalReadPtrData(pMemberData, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);
						_ConvertContext.m_lPatchOffset.Add(SConvertContext::PatchPos(_Writer.Tell(), Offset));	
						_Writer.WritePtr(0x0);
					}
					break;
					case DL_TYPE_STORAGE_PTR:
					{
						pint Offset = DLInternalReadPtrData(pMemberData, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);

						if (Offset != DL_NULL_PTR_OFFSET[_ConvertContext.m_SourcePtrSize])
							_ConvertContext.m_lPatchOffset.Add(SConvertContext::PatchPos(_Writer.Tell(), Offset));

						_Writer.WritePtr(pint(-1));
					}
					break;
					default:
						M_ASSERT(Member.IsSimplePod() || StorageType == DL_TYPE_STORAGE_ENUM);
						_Writer.WriteSwap(pMemberData, Member.m_Size[_ConvertContext.m_SourcePtrSize]);
						break;
				}
			}
			break;
			case DL_TYPE_ATOM_INLINE_ARRAY:
			{
				switch(StorageType)
				{
					case DL_TYPE_STORAGE_STRUCT:
					{
						const SDLType* pSubType = DLFindType(_Context, Member.m_TypeID);
						if(pSubType == 0x0)
							return DL_ERROR_TYPE_NOT_FOUND;

						pint MemberSize  = Member.m_Size[_ConvertContext.m_SourcePtrSize];
						pint SubtypeSize = pSubType->m_Size[_ConvertContext.m_SourcePtrSize];
						for (pint ElemOffset = 0; ElemOffset < MemberSize; ElemOffset += SubtypeSize)
							DLInternalConvertWriteStruct(_Context, pMemberData + ElemOffset, pSubType, _ConvertContext, _Writer);
					}
					break;
					case DL_TYPE_STORAGE_STR:
					{
						pint PtrSizeSource = DLPtrSize(_ConvertContext.m_SourcePtrSize);
						pint PtrSizeTarget = DLPtrSize(_ConvertContext.m_TargetPtrSize);
						uint32 Count = Member.m_Size[_ConvertContext.m_SourcePtrSize] / (uint32)PtrSizeSource;
						pint Pos = _Writer.Tell();

						for (pint iElem = 0; iElem < Count; ++iElem)
						{
							pint OldOffset = DLInternalReadPtrData(pMemberData + (iElem * PtrSizeSource), _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);
							_ConvertContext.m_lPatchOffset.Add(SConvertContext::PatchPos(Pos + (iElem * PtrSizeTarget), OldOffset));
						}

						_Writer.WriteZero(Member.m_Size[_ConvertContext.m_TargetPtrSize]);
					}
					break;
					default:
					{
						M_ASSERT(Member.IsSimplePod() || StorageType == DL_TYPE_STORAGE_ENUM);

						pint   PodSize   = DLPodSize(Member.m_Type);
						uint32 ArraySize = Member.m_Size[_ConvertContext.m_SourcePtrSize];

						switch(PodSize)
						{
							case 1: _Writer.WriteArray((uint8* )pMemberData, ArraySize / sizeof(uint8) );  break;
							case 2: _Writer.WriteArray((uint16*)pMemberData, ArraySize / sizeof(uint16) ); break;
							case 4: _Writer.WriteArray((uint32*)pMemberData, ArraySize / sizeof(uint32) ); break;
							case 8: _Writer.WriteArray((uint64*)pMemberData, ArraySize / sizeof(uint64) ); break;
							default:
								M_ASSERT(false && "Not supported pod-size!");
						}
					}
					break;
				}
			}
			break;

			case DL_TYPE_ATOM_ARRAY:
			{
				pint Offset = 0; uint32 Count = 0;
				DLInternalReadArrayData( pMemberData, &Offset, &Count, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize );

				if(Offset != DL_NULL_PTR_OFFSET[_ConvertContext.m_SourcePtrSize])
					_ConvertContext.m_lPatchOffset.Add(SConvertContext::PatchPos(_Writer.Tell(), Offset));
				else
					Offset = DL_NULL_PTR_OFFSET[_ConvertContext.m_TargetPtrSize];

				_Writer.WritePtr(Offset);
				_Writer.Write(*(uint32*)(pMemberData + DLPtrSize(_ConvertContext.m_SourcePtrSize)));
			}
			break;

			case DL_TYPE_ATOM_BITFIELD:
			{
				uint32 j = iMember;

				do { j++; } while(j < _pType->m_nMembers && _pType->m_lMembers[j].AtomType() == DL_TYPE_ATOM_BITFIELD);

				if(_ConvertContext.m_SourceEndian != _ConvertContext.m_TargetEndian)
				{
					uint32 nBFMembers = j - iMember;

					switch(Member.m_Size[_ConvertContext.m_SourcePtrSize])
					{
						case 1: _Writer.Write( DLConvertBitFieldFormat( *(uint8*)pMemberData, &Member, nBFMembers, _ConvertContext) ); break;
						case 2: _Writer.Write( DLConvertBitFieldFormat(*(uint16*)pMemberData, &Member, nBFMembers, _ConvertContext) ); break;
						case 4: _Writer.Write( DLConvertBitFieldFormat(*(uint32*)pMemberData, &Member, nBFMembers, _ConvertContext) ); break;
						case 8: _Writer.Write( DLConvertBitFieldFormat(*(uint64*)pMemberData, &Member, nBFMembers, _ConvertContext) ); break;
						default:
							M_ASSERT(false && "Not supported pod-size or bitfield-size!");
					}
				}
				else
					_Writer.Write(pMemberData, Member.m_Size[_ConvertContext.m_SourcePtrSize]);

				iMember = j - 1;
			}
			break;

			default:
				M_ASSERT(false && "Invalid ATOM-type!");
		}
	}

	// we need to write our entire size with zeroes. Our entire size might be less than the sum of teh members.
	pint PosDiff = _Writer.Tell() - Pos;

	if(PosDiff < _pType->m_Size[_ConvertContext.m_TargetPtrSize])
		_Writer.WriteZero(_pType->m_Size[_ConvertContext.m_TargetPtrSize] - PosDiff);

	M_ASSERT(_Writer.Tell() - Pos == _pType->m_Size[_ConvertContext.m_TargetPtrSize]);

	return DL_ERROR_OK;
}

static EDLError DLInternalConvertWriteInstance( HDLContext       _Context, 
												const SInstance& _Inst,
												pint*            _pNewOffset,
												SConvertContext& _ConvertContext,
												CDLBinaryWriter& _Writer )
{
	union { const uint8* m_u8; const uint16* m_u16; const uint32* m_u32; const uint64* m_u64; const char* m_str; } Data;
	Data.m_u8 = _Inst.m_pAddress;

	_Writer.SeekEnd(); // place instance at the end!

	if(_Inst.m_pType != 0x0)
		_Writer.Align(_Inst.m_pType->m_Alignment[_ConvertContext.m_TargetPtrSize]);
	
	*_pNewOffset = _Writer.Tell();

	EDLType AtomType    = EDLType(_Inst.m_Type & DL_TYPE_ATOM_MASK);
	EDLType StorageType = EDLType(_Inst.m_Type & DL_TYPE_STORAGE_MASK);

	if(AtomType == DL_TYPE_ATOM_ARRAY)
	{
		switch(StorageType)
		{
			case DL_TYPE_STORAGE_STRUCT:
			{
				pint TypeSize = _Inst.m_pType->m_Size[_ConvertContext.m_SourcePtrSize];
	 			for (pint ElemOffset = 0; ElemOffset < _Inst.m_ArrayCount * TypeSize; ElemOffset += TypeSize)
	 			{
	 				EDLError err = DLInternalConvertWriteStruct(_Context, Data.m_u8 + ElemOffset, _Inst.m_pType, _ConvertContext, _Writer);
	 				if(err != DL_ERROR_OK) return err;
	 			}
			} break;

			case DL_TYPE_STORAGE_STR:
			{
				pint TypeSize = DLPtrSize(_ConvertContext.m_SourcePtrSize);
	 			for(pint ElemOffset = 0; ElemOffset < _Inst.m_ArrayCount * TypeSize; ElemOffset += TypeSize)
	 			{
	 				pint OrigOffset = DLInternalReadPtrData(Data.m_u8 + ElemOffset, _ConvertContext.m_SourceEndian, _ConvertContext.m_SourcePtrSize);
	 				_ConvertContext.m_lPatchOffset.Add(SConvertContext::PatchPos(_Writer.Tell(), OrigOffset));
	 				_Writer.WritePtr(OrigOffset);
	 			}
			} break;

			case DL_TYPE_STORAGE_INT8:  
			case DL_TYPE_STORAGE_UINT8:  _Writer.WriteArray(Data.m_u8, _Inst.m_ArrayCount ); break;
			case DL_TYPE_STORAGE_INT16: 
			case DL_TYPE_STORAGE_UINT16: _Writer.WriteArray(Data.m_u16, _Inst.m_ArrayCount ); break;
			case DL_TYPE_STORAGE_INT32: 
			case DL_TYPE_STORAGE_UINT32: 
			case DL_TYPE_STORAGE_FP32:
			case DL_TYPE_STORAGE_ENUM:   _Writer.WriteArray(Data.m_u32, _Inst.m_ArrayCount ); break;
			case DL_TYPE_STORAGE_INT64: 
			case DL_TYPE_STORAGE_UINT64: 
			case DL_TYPE_STORAGE_FP64:   _Writer.WriteArray(Data.m_u64, _Inst.m_ArrayCount ); break;

			default:
				M_ASSERT(false && "Unknown storage type!");
		}

		return DL_ERROR_OK;
	}

	if(AtomType == DL_TYPE_ATOM_POD && StorageType == DL_TYPE_STORAGE_STR)
	{
		_Writer.WriteArray(Data.m_u8, strlen(Data.m_str) + 1 );
		return DL_ERROR_OK;
	}

	M_ASSERT(AtomType == DL_TYPE_ATOM_POD);
	M_ASSERT(StorageType == DL_TYPE_STORAGE_STRUCT || StorageType == DL_TYPE_STORAGE_PTR);
	return DLInternalConvertWriteStruct(_Context, Data.m_u8, _Inst.m_pType, _ConvertContext, _Writer);
}

EDLError DLInternalConvertNoHeader( HDLContext     _Context,
								    uint8*         _pData, 
									uint8*         _pBaseData,
								    uint8*         _pOutData, 
								    pint           _OutDataSize, 
								    pint*          _pNeededSize,
								    ECpuEndian     _SourceEndian,
								    ECpuEndian     _TargetEndian,
								    EDLPtrSize     _SourcePtrSize,
								    EDLPtrSize     _TargetPtrSize,
								    const SDLType* _pRootType,
									pint           _BaseOffset )
{
	// need a parameter for IsSwapping
	CDLBinaryWriter Writer(_pOutData, _OutDataSize, _pOutData == 0x0, _SourceEndian, _TargetEndian, _TargetPtrSize);
	SConvertContext ConvCtx( _SourceEndian, _TargetEndian, _SourcePtrSize, _TargetPtrSize );

	ConvCtx.m_lInstances.Add(SInstance(_pData, _pRootType, 0x0, EDLType(DL_TYPE_ATOM_POD | DL_TYPE_STORAGE_STRUCT)));
	EDLError err = DLInternalConvertCollectInstances(_Context, _pRootType, _pData, _pBaseData, ConvCtx);

	// TODO: we need to sort the instances here after their offset!

	for(uint i = 0; i < ConvCtx.m_lInstances.Len(); ++i)
	{
		err = DLInternalConvertWriteInstance( _Context, ConvCtx.m_lInstances[i], &ConvCtx.m_lInstances[i].m_OffsetAfterPatch, ConvCtx, Writer);
		if(err != DL_ERROR_OK) 
			return err;
	}

	if(_pOutData != 0x0) // no need to patch data if we are only calculating size
	{
		for(uint i = 0; i < ConvCtx.m_lPatchOffset.Len(); ++i)
		{
			SConvertContext::PatchPos& PP = ConvCtx.m_lPatchOffset[i];

			// find new offset
			pint NewOffset = pint(-1);

			for(uint j = 0; j < ConvCtx.m_lInstances.Len(); ++j )
			{
				pint OldOffset = ConvCtx.m_lInstances[j].m_pAddress - _pBaseData;

				if(OldOffset == PP.m_OldOffset)
				{
					NewOffset = ConvCtx.m_lInstances[j].m_OffsetAfterPatch;
					break;
				}
			}

			M_ASSERT(NewOffset != pint(-1) && "We should have found the instance!");

			Writer.SeekSet(PP.m_Pos);
			Writer.WritePtr(NewOffset + _BaseOffset);
		}
	}

	Writer.SeekEnd();
	*_pNeededSize = Writer.Tell();

	return err;
}

// this function assumes that all error-checking of header etc is done!
static EDLError DLInternalConvertInstance( HDLContext _Context, 
										   uint8*     _pData, 
										   uint8*     _pOutData, 
										   pint       _OutDataSize, 
										   ECpuEndian _Endian, 
										   pint       _PtrSize,
										   pint*      _pNeededSize )
{
	SDLDataHeader* pHeader = (SDLDataHeader*)_pData;

	EDLPtrSize SourcePtrSize = pHeader->m_64BitPtr != 0 ? DL_PTR_SIZE_64BIT : DL_PTR_SIZE_32BIT;
	EDLPtrSize TargetPtrSize;
	
	switch(_PtrSize)
	{
		case 4: TargetPtrSize = DL_PTR_SIZE_32BIT; break;
		case 8: TargetPtrSize = DL_PTR_SIZE_64BIT; break;
		default: return DL_ERROR_INVALID_PARAMETER;
	}

	ECpuEndian SourceEndian = ENDIAN_HOST;
	if (pHeader->m_Id == DL_TYPE_DATA_ID_SWAPED)
		SourceEndian = DLOtherEndian(ENDIAN_HOST);

	bool NeedSwap = SourceEndian != _Endian;

	ECpuEndian TargetEndian = NeedSwap ? _Endian : SourceEndian;

	StrHash RootHash = SourceEndian != ENDIAN_HOST ? DLSwapEndian(pHeader->m_RootInstanceType) : pHeader->m_RootInstanceType;

	const SDLType* pRootType = DLFindType(_Context, RootHash);
	if(pRootType == 0x0)
		return DL_ERROR_TYPE_NOT_FOUND;

	EDLError err = DLInternalConvertNoHeader( _Context, 
											  _pData + sizeof(SDLDataHeader), 
											  _pData + sizeof(SDLDataHeader), 
											  _pOutData == 0x0 ? 0x0 : _pOutData + sizeof(SDLDataHeader), 
											  _OutDataSize - sizeof(SDLDataHeader), 
											  _pNeededSize, 
											  SourceEndian, 
											  TargetEndian, 
											  SourcePtrSize, 
											  TargetPtrSize,
											  pRootType,
											  0 );

	if(_pOutData != 0x0)
	{
		// write new header!
		memcpy(_pOutData, _pData, sizeof(SDLDataHeader));

		SDLDataHeader* pNewHeader = (SDLDataHeader*)_pOutData;

		if(SourcePtrSize != TargetPtrSize)
		{
			pNewHeader->m_64BitPtr ^= 1; // flip ptr-size!

			uint32 NewSize = uint32(*_pNeededSize);

			if(ENDIAN_HOST == SourceEndian)
				pNewHeader->m_InstanceSize = NewSize;
 			else
 				pNewHeader->m_InstanceSize = DLSwapEndian(NewSize);
		}

		if(NeedSwap)
			DLSwapHeader(pNewHeader);
	}
	
	*_pNeededSize += sizeof(SDLDataHeader);
	return err;
}

static inline bool DLInternalDataNeedSwap(SDLDataHeader* _pHeader, ECpuEndian _RequestedEndian)
{
	if(_pHeader->m_Id == DL_TYPE_DATA_ID)
		return _RequestedEndian != ENDIAN_HOST;
	else
		return _RequestedEndian == ENDIAN_HOST;
}

EDLError DLConvertInstanceInplace(HDLContext _Context, uint8* _pData, pint _DataSize, ECpuEndian _Endian, pint _PtrSize)
{
	SDLDataHeader* pHeader = (SDLDataHeader*)_pData;

	if(_DataSize < sizeof(SDLDataHeader))        return DL_ERROR_MALFORMED_DATA;
	if( pHeader->m_Id != DL_TYPE_DATA_ID && 
		pHeader->m_Id != DL_TYPE_DATA_ID_SWAPED) return DL_ERROR_MALFORMED_DATA;
	if(_PtrSize != 4 && _PtrSize != 8)           return DL_ERROR_INVALID_PARAMETER;
	if(pHeader->m_64BitPtr == 0 && _PtrSize > 4) return DL_ERROR_UNSUPORTED_OPERATION;

	pint SourcePtrSize = pHeader->m_64BitPtr != 0 ? 8 : 4;

	bool NeedSwap     = DLInternalDataNeedSwap(pHeader, _Endian);
	bool NeedPtrPatch = SourcePtrSize != _PtrSize;

	if(!NeedSwap && !NeedPtrPatch)
		return DL_ERROR_OK;

	pint NeededSize;
	return DLInternalConvertInstance( _Context, _pData, _pData, _DataSize, _Endian, _PtrSize, &NeededSize);
}

EDLError DLConvertInstance(HDLContext _Context, uint8* _pData, pint _DataSize, uint8* _pOutData, pint _OutDataSize, ECpuEndian _Endian, pint _PtrSize)
{
	SDLDataHeader* pHeader = (SDLDataHeader*)_pData;

	if(_DataSize < sizeof(SDLDataHeader))        return DL_ERROR_MALFORMED_DATA;
	if( pHeader->m_Id != DL_TYPE_DATA_ID && 
		pHeader->m_Id != DL_TYPE_DATA_ID_SWAPED) return DL_ERROR_MALFORMED_DATA;
	if(_PtrSize != 4 && _PtrSize != 8)           return DL_ERROR_INVALID_PARAMETER;

	pint SourcePtrSize = pHeader->m_64BitPtr != 0 ? 8 : 4;

	bool NeedSwap     = DLInternalDataNeedSwap(pHeader, _Endian);
	bool NeedPtrPatch = SourcePtrSize != _PtrSize;

	if(!NeedSwap && !NeedPtrPatch)
	{
		M_ASSERT(_pOutData != _pData && "Src and destination can not be the same!");
		memcpy(_pOutData, _pData, _DataSize);
		return DL_ERROR_OK;
	}

	pint NeededSize;
	return DLInternalConvertInstance( _Context, _pData, _pOutData, _OutDataSize, _Endian, _PtrSize, &NeededSize);
}

EDLError DLInstanceSizeConverted(HDLContext _Context, uint8* _pData, pint _DataSize, pint _PtrSize, pint* _pResultSize)
{
	SDLDataHeader* pHeader = (SDLDataHeader*)_pData;

	if(_DataSize < sizeof(SDLDataHeader))                                             return DL_ERROR_MALFORMED_DATA;
	if( pHeader->m_Id != DL_TYPE_DATA_ID && pHeader->m_Id != DL_TYPE_DATA_ID_SWAPED ) return DL_ERROR_MALFORMED_DATA;

	pint SourcePtrSize = pHeader->m_64BitPtr != 0 ? 8 : 4;

	if(SourcePtrSize == _PtrSize)
	{
		*_pResultSize = _DataSize;
		return DL_ERROR_OK;
	}

	return DLInternalConvertInstance( _Context, _pData, 0x0, 0, ENDIAN_HOST, _PtrSize, _pResultSize);
}